#include "bgpstream_runner/common.h"

#include <curses.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

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

int decode_exit_code(int status) {
    if (status == -1) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
}

}  // namespace

FileProgressDisplay::FileProgressDisplay(std::size_t total_files, std::uint64_t total_bytes)
    : total_files_(total_files), total_bytes_(total_bytes), started_at_(std::chrono::steady_clock::now()) {
    const char *term = std::getenv("TERM");
    use_curses_ = (::isatty(STDOUT_FILENO) == 1 && term != nullptr && std::string_view(term) != "dumb");

    if (use_curses_) {
        initscr();
        cbreak();
        noecho();
        curs_set(0);
        scrollok(stdscr, FALSE);
        render_locked();
    } else {
        render_locked();
    }
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
    const double fraction = total_files_ == 0 ? 1.0 : static_cast<double>(completed_files_) / total_files_;
    const std::size_t filled = static_cast<std::size_t>(fraction * static_cast<double>(kBarWidth));
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at_);

    std::ostringstream output;
    output << "process [";
    for (std::size_t index = 0; index < kBarWidth; ++index) {
        if (index < filled) {
            output << '=';
        } else if (index == filled && completed_files_ < total_files_) {
            output << '>';
        } else {
            output << ' ';
        }
    }
    output << "] " << std::fixed << std::setprecision(1) << (fraction * 100.0) << "% " << completed_files_ << "/"
           << total_files_ << " files " << format_bytes(completed_bytes_) << "/" << format_bytes(total_bytes_)
           << " elapsed=" << format_elapsed(elapsed);
    return output.str();
}

void FileProgressDisplay::render_locked() {
    const std::string line = build_line_locked();
    last_line_ = line;

    if (use_curses_ && !closed_) {
        erase();
        attron(A_BOLD);
        mvaddnstr(0, 0, line.c_str(), COLS > 0 ? COLS - 1 : static_cast<int>(line.size()));
        attroff(A_BOLD);
        clrtoeol();
        refresh();
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

    if (use_curses_) {
        endwin();
        std::cout << last_line_ << '\n';
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
           << "  --output-dir PATH\n"
           << "  --download-workers N\n"
           << "  --parser-workers N\n"
           << "  --message-batch-size N\n"
           << "  --chunk-size N\n"
           << "  --chunk-unit day|month\n"
           << "  --max-cache-size-gb NUMBER\n"
           << "  --log-phase-transitions true|false\n"
           << "  --log-chunk-summary true|false\n"
           << "  --log-final-summary true|false\n"
           << "  --limit N\n"
           << "  --help\n";
    std::exit(exit_code);
}

Config parse_args(int argc, char **argv) {
    Config config;
    const std::filesystem::path config_file = repo_config_path();
    if (!std::filesystem::exists(config_file)) {
        throw std::runtime_error("Required config file does not exist: " + config_file.string());
    }
    apply_json_config_file(config_file, &config);

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
        } else if (arg == "--output-dir") {
            config.output_dir = require_value("--output-dir");
        } else if (arg == "--download-workers") {
            config.download_workers = std::stoi(require_value("--download-workers"));
        } else if (arg == "--parser-workers") {
            config.parser_workers = std::stoi(require_value("--parser-workers"));
        } else if (arg == "--message-batch-size") {
            config.message_batch_size = std::stoi(require_value("--message-batch-size"));
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
        } else if (arg == "--max-cache-size-gb") {
            config.max_cache_size_gb = std::stod(require_value("--max-cache-size-gb"));
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
    if (config.max_cache_size_gb <= 0) {
        throw std::runtime_error("--max-cache-size-gb must be greater than 0");
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

std::string shell_escape(std::string_view value) {
    std::string escaped = "'";
    for (char ch : value) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('\'');
    return escaped;
}

CommandResult run_capture_command(const std::string &command) {
    const std::string redirected_command = command + " 2>&1";
    FILE *pipe = popen(redirected_command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("Failed to execute command: " + command);
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    const int status = pclose(pipe);
    CommandResult result;
    result.exit_code = decode_exit_code(status);
    result.output = std::move(output);
    return result;
}

int run_streaming_command(const std::string &command) { return decode_exit_code(std::system(command.c_str())); }

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
