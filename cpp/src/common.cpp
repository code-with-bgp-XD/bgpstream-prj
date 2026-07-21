#include "bgpstream_runner/common.h"

#include <signal.h>
#include <sys/ioctl.h>
#include <term.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "bgpstream_runner/config_file.h"

namespace bgpstream_runner {

namespace {

#ifndef BGPSTREAM_SOURCE_DIR
#define BGPSTREAM_SOURCE_DIR "."
#endif

std::time_t parse_utc_date(const std::string &date_text) {
    std::tm tm{};
    std::istringstream input(date_text);
    input >> std::get_time(&tm, "%Y-%m-%d");
    if (input.fail()) {
        throw std::runtime_error("Unsupported date format: " + date_text);
    }
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = 0;

    const std::time_t epoch = timegm(&tm);
    if (epoch == static_cast<std::time_t>(-1)) {
        throw std::runtime_error("Failed to convert UTC date to epoch: " + date_text);
    }
    return epoch;
}

std::time_t first_day_of_next_month(std::time_t epoch, int month_step) {
    std::tm tm{};
    if (gmtime_r(&epoch, &tm) == nullptr) {
        throw std::runtime_error("Failed to convert UTC timestamp");
    }

    tm.tm_mday = 1;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_mon += month_step;

    const std::time_t next_epoch = timegm(&tm);
    if (next_epoch == static_cast<std::time_t>(-1)) {
        throw std::runtime_error("Failed to compute next month boundary");
    }
    return next_epoch;
}

std::time_t add_days(std::time_t epoch, int day_step) {
    static constexpr std::time_t kSecondsPerDay = 24 * 60 * 60;
    return epoch + static_cast<std::time_t>(day_step) * kSecondsPerDay;
}

std::filesystem::path repo_config_path() {
    return std::filesystem::path(BGPSTREAM_SOURCE_DIR) / kDefaultConfigPath;
}

thread_local std::string *terminal_capability_output = nullptr;

int append_terminal_capability_character(int character) {
    if (terminal_capability_output != nullptr) {
        terminal_capability_output->push_back(static_cast<char>(character));
    }
    return character;
}

bool is_valid_terminal_capability(const char *capability) {
    return capability != nullptr && capability != reinterpret_cast<const char *>(-1);
}

std::string terminal_capability(const char *name) {
    const char *capability = tigetstr(name);
    return is_valid_terminal_capability(capability) ? capability : "";
}

void append_terminal_capability(std::string *output, const std::string &capability) {
    if (capability.empty()) {
        return;
    }

    std::string *const previous_output = terminal_capability_output;
    terminal_capability_output = output;
    tputs(capability.c_str(), 1, append_terminal_capability_character);
    terminal_capability_output = previous_output;
}

void append_terminal_capability(std::string *output, const std::string &capability, int first_parameter,
                                int second_parameter) {
    if (capability.empty()) {
        return;
    }

    const char *expanded = tparm(capability.c_str(), static_cast<long>(first_parameter),
                                 static_cast<long>(second_parameter));
    if (is_valid_terminal_capability(expanded)) {
        append_terminal_capability(output, expanded);
    }
}

void write_terminal_output(std::string_view output) {
    std::size_t written = 0;
    while (written < output.size()) {
        const ssize_t result = ::write(STDOUT_FILENO, output.data() + written, output.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

bool query_terminal_size(int *row_count, int *column_count) {
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row >= 2 && size.ws_col >= 2) {
        *row_count = static_cast<int>(size.ws_row);
        *column_count = static_cast<int>(size.ws_col);
        return true;
    }

    const int terminfo_rows = tigetnum("lines");
    const int terminfo_columns = tigetnum("cols");
    if (terminfo_rows < 2 || terminfo_columns < 2) {
        return false;
    }

    *row_count = terminfo_rows;
    *column_count = terminfo_columns;
    return true;
}

void restore_terminal_on_signal(int signal_number) {
    static constexpr char reset_terminal[] = "\x1b[r\x1b[999;1H\x1b[2K\r\n";
    const ssize_t ignored = ::write(STDOUT_FILENO, reset_terminal, sizeof(reset_terminal) - 1);
    (void)ignored;

    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigaction(signal_number, &default_action, nullptr);
    if (::kill(::getpid(), signal_number) != 0) {
        _exit(128 + signal_number);
    }
}

}  // namespace

struct FileProgressDisplay::TerminalState {
    struct SignalHandlerRegistration {
        int signal_number = 0;
        struct sigaction previous_action {};
    };

    TERMINAL *terminfo_terminal = nullptr;
    int row_count = 0;
    int column_count = 0;
    std::string cap_cursor_address;
    std::string cap_change_scroll_region;
    std::string cap_save_cursor;
    std::string cap_restore_cursor;
    std::string cap_clear_to_end_of_line;
    std::string cap_enter_bold_mode;
    std::string cap_exit_attribute_mode;
    std::vector<SignalHandlerRegistration> signal_handlers;
};

FileProgressDisplay::FileProgressDisplay(std::size_t total_files, std::uint64_t total_bytes,
                                         std::string phase, std::string item_label, bool show_bytes)
    : total_files_(total_files),
      total_bytes_(total_bytes),
      phase_(std::move(phase)),
      item_label_(std::move(item_label)),
      show_bytes_(show_bytes),
      started_at_(std::chrono::steady_clock::now()) {
    initialize_terminal_locked();
    render_locked();
}

FileProgressDisplay::~FileProgressDisplay() { close(); }

void FileProgressDisplay::mark_batch_completed(std::size_t completed_files, std::uint64_t completed_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    completed_files_ += completed_files;
    completed_bytes_ += completed_bytes;
    render_locked();
}

void FileProgressDisplay::finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

void FileProgressDisplay::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

std::string FileProgressDisplay::build_line_locked() const {
    static constexpr std::size_t kBarWidth = 36;
    const double fraction =
        total_files_ == 0 ? 1.0 : static_cast<double>(completed_files_) / static_cast<double>(total_files_);
    const double displayed_percentage =
        completed_files_ < total_files_ ? std::min(fraction * 100.0, 99.9) : 100.0;
    const std::size_t filled = static_cast<std::size_t>(fraction * static_cast<double>(kBarWidth));
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at_);

    std::ostringstream output;
    output << phase_ << " [";
    for (std::size_t index = 0; index < kBarWidth; ++index) {
        if (index < filled) {
            output << '=';
        } else if (index == filled && completed_files_ < total_files_) {
            output << '>';
        } else {
            output << ' ';
        }
    }
    output << "] " << std::fixed << std::setprecision(1) << displayed_percentage << "% "
           << completed_files_ << "/" << total_files_ << " " << item_label_;
    if (show_bytes_) {
        output << " " << format_bytes(completed_bytes_);
        if (total_bytes_ > 0) {
            output << "/" << format_bytes(total_bytes_);
        }
    }
    output << " elapsed=" << format_elapsed(elapsed);
    return output.str();
}

bool FileProgressDisplay::initialize_terminal_locked() {
    const char *term = std::getenv("TERM");
    if (::isatty(STDOUT_FILENO) != 1 || term == nullptr || std::string_view(term) == "dumb") {
        return false;
    }

    std::cout << std::flush;
    std::cerr << std::flush;

    int setup_error = 0;
    if (setupterm(nullptr, STDOUT_FILENO, &setup_error) != 0) {
        return false;
    }

    auto state = std::make_unique<TerminalState>();
    state->terminfo_terminal = cur_term;
    state->cap_cursor_address = terminal_capability("cup");
    state->cap_change_scroll_region = terminal_capability("csr");
    state->cap_save_cursor = terminal_capability("sc");
    state->cap_restore_cursor = terminal_capability("rc");
    state->cap_clear_to_end_of_line = terminal_capability("el");
    state->cap_enter_bold_mode = terminal_capability("bold");
    state->cap_exit_attribute_mode = terminal_capability("sgr0");

    if (state->cap_cursor_address.empty() || state->cap_change_scroll_region.empty() ||
        state->cap_save_cursor.empty() || state->cap_restore_cursor.empty() ||
        state->cap_clear_to_end_of_line.empty() ||
        !query_terminal_size(&state->row_count, &state->column_count)) {
        del_curterm(state->terminfo_terminal);
        return false;
    }

    static constexpr int kTerminationSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    state->signal_handlers.reserve(4);
    for (const int signal_number : kTerminationSignals) {
        struct sigaction previous_action {};
        if (sigaction(signal_number, nullptr, &previous_action) != 0 || previous_action.sa_handler != SIG_DFL) {
            continue;
        }

        struct sigaction terminal_action {};
        terminal_action.sa_handler = restore_terminal_on_signal;
        sigemptyset(&terminal_action.sa_mask);
        if (sigaction(signal_number, &terminal_action, nullptr) == 0) {
            state->signal_handlers.push_back({signal_number, previous_action});
        }
    }

    // Keep the terminal's main screen active. The upper region remains the normal
    // stdout/stderr log area while the final row is reserved for progress.
    std::string output;
    append_terminal_capability(&output, state->cap_change_scroll_region, 0, state->row_count - 2);
    append_terminal_capability(&output, state->cap_cursor_address, state->row_count - 2, 0);
    write_terminal_output(output);
    terminal_ = std::move(state);
    return true;
}

bool FileProgressDisplay::update_terminal_size_locked() {
    if (terminal_ == nullptr) {
        return false;
    }

    int row_count = 0;
    int column_count = 0;
    if (!query_terminal_size(&row_count, &column_count)) {
        return false;
    }
    if (row_count == terminal_->row_count && column_count == terminal_->column_count) {
        return true;
    }

    std::string output;
    if (terminal_->row_count <= row_count) {
        append_terminal_capability(&output, terminal_->cap_cursor_address, terminal_->row_count - 1, 0);
        append_terminal_capability(&output, terminal_->cap_clear_to_end_of_line);
    }
    append_terminal_capability(&output, terminal_->cap_change_scroll_region, 0, row_count - 2);
    append_terminal_capability(&output, terminal_->cap_cursor_address, row_count - 2, 0);
    write_terminal_output(output);

    terminal_->row_count = row_count;
    terminal_->column_count = column_count;
    return true;
}

void FileProgressDisplay::render_terminal_locked(const std::string &line) {
    update_terminal_size_locked();

    const std::size_t maximum_width = static_cast<std::size_t>(std::max(1, terminal_->column_count - 1));
    const std::string_view visible_line(line.data(), std::min(line.size(), maximum_width));

    // Restore the log cursor after drawing so ordinary stdout/stderr writes keep
    // scrolling above the progress row.
    std::string output;
    append_terminal_capability(&output, terminal_->cap_save_cursor);
    append_terminal_capability(&output, terminal_->cap_cursor_address, terminal_->row_count - 1, 0);
    if (!terminal_->cap_enter_bold_mode.empty() && !terminal_->cap_exit_attribute_mode.empty()) {
        append_terminal_capability(&output, terminal_->cap_enter_bold_mode);
    }
    output.append(visible_line);
    if (!terminal_->cap_enter_bold_mode.empty() && !terminal_->cap_exit_attribute_mode.empty()) {
        append_terminal_capability(&output, terminal_->cap_exit_attribute_mode);
    }
    append_terminal_capability(&output, terminal_->cap_clear_to_end_of_line);
    append_terminal_capability(&output, terminal_->cap_restore_cursor);
    write_terminal_output(output);
}

void FileProgressDisplay::close_terminal_locked() {
    update_terminal_size_locked();
    render_terminal_locked(last_line_);

    std::string output;
    append_terminal_capability(&output, terminal_->cap_change_scroll_region, 0, terminal_->row_count - 1);
    append_terminal_capability(&output, terminal_->cap_cursor_address, terminal_->row_count - 1, 0);
    output += "\r\n";
    write_terminal_output(output);
    for (auto registration = terminal_->signal_handlers.rbegin();
         registration != terminal_->signal_handlers.rend(); ++registration) {
        sigaction(registration->signal_number, &registration->previous_action, nullptr);
    }
    TERMINAL *const terminfo_terminal = terminal_->terminfo_terminal;
    terminal_.reset();
    del_curterm(terminfo_terminal);
}

void FileProgressDisplay::render_locked() {
    const std::string line = build_line_locked();
    last_line_ = line;

    if (terminal_ != nullptr && !closed_) {
        render_terminal_locked(line);
        return;
    }

    const std::size_t padding = last_rendered_width_ > line.size() ? last_rendered_width_ - line.size() : 0;
    std::cout << '\r' << line;
    if (padding > 0) {
        std::cout << std::string(padding, ' ');
    }
    std::cout << std::flush;
    last_rendered_width_ = line.size();
}

void FileProgressDisplay::close_locked() {
    if (closed_) {
        return;
    }
    closed_ = true;

    if (terminal_ != nullptr) {
        close_terminal_locked();
        return;
    }

    std::cout << '\n';
}

[[noreturn]] void print_usage_and_exit(const char *program, int exit_code) {
    std::ostream &stream = exit_code == 0 ? std::cout : std::cerr;
    stream << "Usage: " << program << " [options]\n"
           << "  --start-date YYYY-MM-DD\n"
           << "  --end-date YYYY-MM-DD\n"
           << "  --project NAME\n"
           << "  --collector NAME\n"
           << "  --processor-plugin NAME_OR_PATH\n"
           << "  --download-workers N\n"
           << "  --parser-workers N\n"
           << "  --message-batch-size N\n"
           << "  --parse-on-cache-miss true|false\n"
           << "  --chunk-size N\n"
           << "  --chunk-unit day|month\n"
           << "  --log-phase-transitions true|false\n"
           << "  --log-chunk-summary true|false\n"
           << "  --log-final-summary true|false\n"
           << "  --limit N\n"
           << "  --download-only  Download missing MRT files and generate parsed caches\n"
           << "  --help\n";
    std::exit(exit_code);
}

Config parse_args(int argc, char **argv) {
    Config config;
    const std::filesystem::path config_file = repo_config_path();
    bool download_only_requested = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--download-only") {
            download_only_requested = true;
            break;
        }
    }

    if (!std::filesystem::exists(config_file)) {
        throw std::runtime_error("Required config file does not exist: " + config_file.string());
    }
    apply_json_config_file(config_file, &config);
    // Raw MRT files and their derived parsed caches share the cache root in
    // both modes. `analysis` intentionally has no separate output_dir key.
    config.output_dir = config.cache.output_dir;

    if (download_only_requested) {
        config.start_date = config.cache.start_date;
        config.end_date = config.cache.end_date;
        config.project = config.cache.project;
        config.collector = config.cache.collector;
        config.download_workers = config.cache.download_workers;
        config.limit = config.cache.limit;
        config.parser_workers = config.cache.parser_workers;
        config.message_batch_size = config.cache.message_batch_size;
        config.download_only = true;
    }

    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto require_value = [&](const char *name) -> std::string {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string("Missing value for ") + name);
            }
            return argv[++index];
        };

        if (arg == "--start-date") {
            config.start_date = require_value("--start-date");
        } else if (arg == "--end-date") {
            config.end_date = require_value("--end-date");
        } else if (arg == "--project") {
            config.project = require_value("--project");
        } else if (arg == "--collector") {
            config.collector = require_value("--collector");
        } else if (arg == "--processor-plugin") {
            config.processor_plugin = require_value("--processor-plugin");
        } else if (arg == "--download-workers") {
            config.download_workers = std::stoi(require_value("--download-workers"));
        } else if (arg == "--parser-workers") {
            config.parser_workers = std::stoi(require_value("--parser-workers"));
        } else if (arg == "--message-batch-size") {
            config.message_batch_size = std::stoi(require_value("--message-batch-size"));
        } else if (arg == "--parse-on-cache-miss") {
            const std::string value = require_value("--parse-on-cache-miss");
            if (value == "true") {
                config.parse_on_cache_miss = true;
            } else if (value == "false") {
                config.parse_on_cache_miss = false;
            } else {
                throw std::runtime_error("--parse-on-cache-miss must be true or false");
            }
        } else if (arg == "--chunk-size") {
            config.chunk_size = std::stoi(require_value("--chunk-size"));
        } else if (arg == "--chunk-unit") {
            const std::string value = require_value("--chunk-unit");
            if (value == "day" || value == "days") {
                config.chunk_unit = ChunkUnit::Day;
            } else if (value == "month" || value == "months") {
                config.chunk_unit = ChunkUnit::Month;
            } else {
                throw std::runtime_error("--chunk-unit must be day or month");
            }
        } else if (arg == "--log-phase-transitions") {
            const std::string value = require_value("--log-phase-transitions");
            if (value == "true") {
                config.log_phase_transitions = true;
            } else if (value == "false") {
                config.log_phase_transitions = false;
            } else {
                throw std::runtime_error("--log-phase-transitions must be true or false");
            }
        } else if (arg == "--log-chunk-summary") {
            const std::string value = require_value("--log-chunk-summary");
            if (value == "true") {
                config.log_chunk_summary = true;
            } else if (value == "false") {
                config.log_chunk_summary = false;
            } else {
                throw std::runtime_error("--log-chunk-summary must be true or false");
            }
        } else if (arg == "--log-final-summary") {
            const std::string value = require_value("--log-final-summary");
            if (value == "true") {
                config.log_final_summary = true;
            } else if (value == "false") {
                config.log_final_summary = false;
            } else {
                throw std::runtime_error("--log-final-summary must be true or false");
            }
        } else if (arg == "--limit") {
            config.limit = std::stoi(require_value("--limit"));
        } else if (arg == "--download-only") {
            config.download_only = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage_and_exit(argv[0], 0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (config.download_workers < 1) {
        throw std::runtime_error("--download-workers must be at least 1");
    }
    if (config.parser_workers < 1) {
        throw std::runtime_error("--parser-workers must be at least 1");
    }
    if (config.message_batch_size < 1) {
        throw std::runtime_error("--message-batch-size must be at least 1");
    }
    if (config.chunk_size < 1) {
        throw std::runtime_error("--chunk-size must be at least 1");
    }
    if (config.limit == 0 || config.limit < -1) {
        throw std::runtime_error("--limit must be positive, or omitted");
    }

    return config;
}

std::string trim(std::string value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string format_bytes(std::uint64_t num_bytes) {
    static constexpr const char *kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(num_bytes);
    std::size_t unit_index = 0;
    while (value >= 1024.0 && unit_index + 1 < std::size(kUnits)) {
        value /= 1024.0;
        ++unit_index;
    }

    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << value << ' ' << kUnits[unit_index];
    return output.str();
}

std::string format_elapsed(std::chrono::seconds elapsed) {
    const auto total_seconds = elapsed.count();
    const auto hours = total_seconds / 3600;
    const auto minutes = (total_seconds % 3600) / 60;
    const auto seconds = total_seconds % 60;

    std::ostringstream output;
    output << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes << ':' << std::setw(2)
           << seconds;
    return output.str();
}

std::uint64_t safe_file_size(const std::filesystem::path &file_path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file_path, error);
    if (error) {
        return 0;
    }
    return size;
}

std::uint64_t total_file_bytes(const std::vector<std::filesystem::path> &files) {
    std::uint64_t total_bytes = 0;
    for (const auto &file_path : files) {
        total_bytes += safe_file_size(file_path);
    }
    return total_bytes;
}

ClosedDateRange parse_closed_date_range(const Config &config) {
    const std::time_t start = parse_utc_date(config.start_date);
    const std::time_t end_inclusive = parse_utc_date(config.end_date);
    if (end_inclusive < start) {
        throw std::runtime_error("END_DATE must be greater than or equal to START_DATE");
    }

    ClosedDateRange range;
    range.start_epoch = start;
    range.end_exclusive_epoch = end_inclusive + 24 * 60 * 60;
    return range;
}

std::string format_utc_timestamp(std::time_t epoch) {
    std::tm tm{};
    if (gmtime_r(&epoch, &tm) == nullptr) {
        throw std::runtime_error("Failed to format UTC timestamp");
    }

    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%d %H:%M:%S UTC");
    return output.str();
}

std::filesystem::path utc_year_month_path(std::time_t epoch) {
    std::tm tm{};
    if (gmtime_r(&epoch, &tm) == nullptr) {
        throw std::runtime_error("Failed to determine UTC year and month");
    }

    std::ostringstream year;
    year << std::setfill('0') << std::setw(4) << tm.tm_year + 1900;
    std::ostringstream month;
    month << std::setfill('0') << std::setw(2) << tm.tm_mon + 1;
    return std::filesystem::path(year.str()) / month.str();
}

std::vector<ClosedDateRange> split_range_by_chunks(const ClosedDateRange &range, int chunk_size, ChunkUnit chunk_unit) {
    std::vector<ClosedDateRange> chunks;
    for (std::time_t chunk_start = range.start_epoch; chunk_start < range.end_exclusive_epoch;) {
        std::time_t next_boundary = 0;
        switch (chunk_unit) {
            case ChunkUnit::Day:
                next_boundary = add_days(chunk_start, chunk_size);
                break;
            case ChunkUnit::Month:
                next_boundary = first_day_of_next_month(chunk_start, chunk_size);
                break;
        }

        const std::time_t chunk_end = std::min(next_boundary, range.end_exclusive_epoch);
        chunks.push_back(ClosedDateRange{chunk_start, chunk_end});
        chunk_start = chunk_end;
    }
    return chunks;
}

std::string_view chunk_unit_to_string(ChunkUnit chunk_unit) {
    switch (chunk_unit) {
        case ChunkUnit::Day:
            return "day";
        case ChunkUnit::Month:
            return "month";
    }
    return "unknown";
}

std::string format_range_label(const ClosedDateRange &range) {
    return "[" + format_utc_timestamp(range.start_epoch) + ", " + format_utc_timestamp(range.end_exclusive_epoch) + ")";
}

}  // namespace bgpstream_runner
