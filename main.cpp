extern "C" {
#include <bgpstream.h>
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

#include <curses.h>

namespace fs = std::filesystem;

namespace {

constexpr const char *kStartDate = "2025-11-01";
constexpr const char *kEndDate = "2025-12-01";
constexpr const char *kProject = "routeviews";
constexpr const char *kCollector = "route-views.sg";
constexpr const char *kDataRoot = "bgpdata";
constexpr int kDownloadWorkers = 32;
constexpr int kDefaultStatsBatchSize = 5;

int default_stats_workers() {
  return 8;
}

struct Config {
  std::string start_date = kStartDate;
  std::string end_date = kEndDate;
  std::string project = kProject;
  std::string collector = kCollector;
  fs::path output_dir = kDataRoot;
  int download_workers = kDownloadWorkers;
  int stats_workers = default_stats_workers();
  int stats_batch_size = kDefaultStatsBatchSize;
  int limit = -1;
};

struct ClosedDateRange {
  std::time_t start_epoch{};
  std::time_t end_exclusive_epoch{};
};

struct CommandResult {
  int exit_code = 0;
  std::string output;
};

struct FileStats {
  std::unordered_map<std::string, std::unordered_set<uint32_t>> prefix_to_ases;
  std::uint64_t scanned_update_count = 0;
  std::uint64_t usable_update_count = 0;
};

struct StatsSummary {
  std::uint64_t scanned_update_count = 0;
  std::uint64_t usable_update_count = 0;
  std::uint64_t skipped_parse_files = 0;
  std::uint64_t unique_prefixes = 0;
  std::uint64_t prefix_scoped_as_total = 0;
};

constexpr std::size_t kAggregateShardCount = 64;

struct AggregateShard {
  std::mutex mutex;
  std::unordered_map<std::string, std::unordered_set<uint32_t>> prefix_to_ases;
};

struct ConcurrentAggregate {
  std::vector<AggregateShard> shards = std::vector<AggregateShard>(kAggregateShardCount);
  std::atomic<std::uint64_t> scanned_update_count{0};
  std::atomic<std::uint64_t> usable_update_count{0};
  std::atomic<std::uint64_t> skipped_parse_files{0};
};

std::string format_bytes(std::uint64_t num_bytes);
std::string format_elapsed(std::chrono::seconds elapsed);

class StatsProgressDisplay {
 public:
  StatsProgressDisplay(std::size_t total_files, std::uint64_t total_bytes)
      : total_files_(total_files),
        total_bytes_(total_bytes),
        started_at_(std::chrono::steady_clock::now()) {
    const char *term = std::getenv("TERM");
    use_curses_ = (::isatty(STDOUT_FILENO) == 1 && term != nullptr &&
                   std::string_view(term) != "dumb");

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

  StatsProgressDisplay(const StatsProgressDisplay &) = delete;
  StatsProgressDisplay &operator=(const StatsProgressDisplay &) = delete;

  ~StatsProgressDisplay() {
    close();
  }

  void mark_batch_completed(std::size_t completed_files, std::uint64_t completed_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    completed_files_ += completed_files;
    completed_bytes_ += completed_bytes;
    render_locked();
  }

  void finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
  }

  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
  }

 private:
  std::string build_line_locked() const {
    static constexpr std::size_t kBarWidth = 36;
    const double fraction =
        total_files_ == 0 ? 1.0 : static_cast<double>(completed_files_) / total_files_;
    const std::size_t filled =
        static_cast<std::size_t>(fraction * static_cast<double>(kBarWidth));
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started_at_);

    std::ostringstream output;
    output << "stats [";
    for (std::size_t index = 0; index < kBarWidth; ++index) {
      if (index < filled) {
        output << '=';
      } else if (index == filled && completed_files_ < total_files_) {
        output << '>';
      } else {
        output << ' ';
      }
    }
    output << "] " << std::fixed << std::setprecision(1) << (fraction * 100.0) << "% "
           << completed_files_ << "/" << total_files_ << " files "
           << format_bytes(completed_bytes_) << "/" << format_bytes(total_bytes_)
           << " elapsed=" << format_elapsed(elapsed);
    return output.str();
  }

  void render_locked() {
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

    const std::size_t padding =
        last_rendered_width_ > line.size() ? last_rendered_width_ - line.size() : 0;
    std::cout << '\r' << line;
    if (padding > 0) {
      std::cout << std::string(padding, ' ');
    }
    std::cout << std::flush;
    last_rendered_width_ = line.size();
  }

  void close_locked() {
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

  const std::size_t total_files_;
  const std::uint64_t total_bytes_;
  const std::chrono::steady_clock::time_point started_at_;
  std::mutex mutex_;
  std::size_t completed_files_ = 0;
  std::uint64_t completed_bytes_ = 0;
  std::size_t last_rendered_width_ = 0;
  std::string last_line_;
  bool use_curses_ = false;
  bool closed_ = false;
};

[[noreturn]] void print_usage_and_exit(const char *program, int exit_code) {
  std::ostream &stream = exit_code == 0 ? std::cout : std::cerr;
  stream
      << "Usage: " << program << " [options]\n"
      << "  --start-date YYYY-MM-DD\n"
      << "  --end-date YYYY-MM-DD\n"
      << "  --project NAME\n"
      << "  --collector NAME\n"
      << "  --output-dir PATH\n"
      << "  --download-workers N\n"
      << "  --stats-workers N\n"
      << "  --stats-batch-size N\n"
      << "  --limit N\n"
      << "  --help\n";
  std::exit(exit_code);
}

Config parse_args(int argc, char **argv) {
  Config config;

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
    } else if (arg == "--output-dir") {
      config.output_dir = require_value("--output-dir");
    } else if (arg == "--download-workers") {
      config.download_workers = std::stoi(require_value("--download-workers"));
    } else if (arg == "--stats-workers") {
      config.stats_workers = std::stoi(require_value("--stats-workers"));
    } else if (arg == "--stats-batch-size") {
      config.stats_batch_size = std::stoi(require_value("--stats-batch-size"));
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
  if (config.stats_workers < 1) {
    throw std::runtime_error("--stats-workers must be at least 1");
  }
  if (config.stats_batch_size < 1) {
    throw std::runtime_error("--stats-batch-size must be at least 1");
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
  output << std::setfill('0') << std::setw(2) << hours << ':'
         << std::setw(2) << minutes << ':'
         << std::setw(2) << seconds;
  return output.str();
}

std::uint64_t safe_file_size(const fs::path &file_path) {
  std::error_code error;
  const auto size = fs::file_size(file_path, error);
  if (error) {
    return 0;
  }
  return size;
}

std::uint64_t total_file_bytes(const std::vector<fs::path> &files) {
  std::uint64_t total_bytes = 0;
  for (const fs::path &file_path : files) {
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

CommandResult run_capture_command(const std::string &command) {
  FILE *pipe = popen(command.c_str(), "r");
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

int run_streaming_command(const std::string &command) {
  return decode_exit_code(std::system(command.c_str()));
}

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

std::string detect_python_executable() {
  const fs::path venv_python = fs::path(".venv") / "bin" / "python";
  if (fs::exists(venv_python)) {
    return venv_python.string();
  }
  return "python3";
}

std::string build_download_command(const Config &config,
                                   const ClosedDateRange &range,
                                   bool dry_run) {
  std::ostringstream command;
  command << shell_escape(detect_python_executable()) << " "
          << shell_escape("download.py")
          << " --from-time " << shell_escape(format_utc_timestamp(range.start_epoch))
          << " --until-time " << shell_escape(format_utc_timestamp(range.end_exclusive_epoch))
          << " --collector " << shell_escape(config.collector)
          << " --project " << shell_escape(config.project)
          << " --record-type updates"
          << " --source auto"
          << " --workers " << config.download_workers
          << " --output-dir " << shell_escape(config.output_dir.string());
  if (config.limit > 0) {
    command << " --limit " << config.limit;
  }
  if (dry_run) {
    command << " --dry-run";
  }
  return command.str();
}

std::vector<fs::path> collect_target_files(const Config &config,
                                           const ClosedDateRange &range) {
  const CommandResult result =
      run_capture_command(build_download_command(config, range, true));
  if (result.exit_code != 0) {
    throw std::runtime_error("download.py --dry-run failed with exit code " +
                             std::to_string(result.exit_code) + "\n" + result.output);
  }

  std::vector<fs::path> files;
  std::istringstream lines(result.output);
  std::string line;
  while (std::getline(lines, line)) {
    const auto arrow = line.rfind(" -> ");
    if (arrow == std::string::npos) {
      continue;
    }
    const std::string path_text = trim(line.substr(arrow + 4));
    if (!path_text.empty()) {
      files.emplace_back(path_text);
    }
  }

  if (files.empty()) {
    throw std::runtime_error("No target files were discovered from download.py --dry-run");
  }
  return files;
}

std::string record_status_to_string(bgpstream_record_status_t status) {
  char buffer[128];
  if (bgpstream_record_status_snprintf(buffer, sizeof(buffer), status) < 0) {
    return "unknown-record-status";
  }
  return std::string(buffer);
}

std::string format_parse_failure(const fs::path &file_path, const std::string &reason) {
  std::ostringstream output;
  output << "解析跳过: " << file_path.filename().string() << " | " << reason;
  return output.str();
}

void add_asns_from_path(const bgpstream_as_path_t *as_path,
                        std::unordered_set<uint32_t> *ases) {
  if (as_path == nullptr) {
    return;
  }

  bgpstream_as_path_iter_t iter;
  bgpstream_as_path_iter_reset(&iter);

  while (bgpstream_as_path_seg_t *seg =
             bgpstream_as_path_get_next_seg(as_path, &iter)) {
    switch (seg->type) {
      case BGPSTREAM_AS_PATH_SEG_ASN:
        ases->insert(seg->asn.asn);
        break;
      case BGPSTREAM_AS_PATH_SEG_SET:
      case BGPSTREAM_AS_PATH_SEG_CONFED_SEQ:
      case BGPSTREAM_AS_PATH_SEG_CONFED_SET:
        for (uint8_t index = 0; index < seg->set.asn_cnt; ++index) {
          ases->insert(seg->set.asn[index]);
        }
        break;
      default:
        break;
    }
  }
}

std::string prefix_to_string(const bgpstream_pfx_t &prefix) {
  char buffer[128];
  if (bgpstream_pfx_snprintf(buffer, sizeof(buffer), &prefix) == nullptr) {
    throw std::runtime_error(std::string("Failed to stringify prefix: ") +
                             std::strerror(errno));
  }
  return std::string(buffer);
}

FileStats collect_file_prefix_stats(const fs::path &file_path,
                                    std::time_t start_epoch,
                                    std::time_t end_exclusive_epoch) {
  bgpstream_t *stream = bgpstream_create();
  if (stream == nullptr) {
    throw std::runtime_error("Failed to create BGPStream instance");
  }

  auto destroy_stream = [&stream]() {
    if (stream != nullptr) {
      bgpstream_destroy(stream);
      stream = nullptr;
    }
  };

  try {
    const auto interface_id =
        bgpstream_get_data_interface_id_by_name(stream, "singlefile");
    if (interface_id == _BGPSTREAM_DATA_INTERFACE_INVALID) {
      throw std::runtime_error("singlefile data interface is unavailable");
    }

    bgpstream_set_data_interface(stream, interface_id);

    bgpstream_data_interface_option_t *upd_file_option =
        bgpstream_get_data_interface_option_by_name(stream, interface_id, "upd-file");
    if (upd_file_option == nullptr) {
      throw std::runtime_error("singlefile/upd-file option is unavailable");
    }

    if (bgpstream_set_data_interface_option(
            stream, upd_file_option, file_path.string().c_str()) != 0) {
      throw std::runtime_error("Failed to set upd-file option for " + file_path.string());
    }

    if (bgpstream_start(stream) != 0) {
      throw std::runtime_error("Failed to start BGPStream for " + file_path.string());
    }

    FileStats stats;
    bgpstream_record_t *record = nullptr;

    while (true) {
      const int record_rc = bgpstream_get_next_record(stream, &record);
      if (record_rc == 0) {
        break;
      }
      if (record_rc < 0) {
        throw std::runtime_error("Failed to read BGP record stream");
      }

      if (record == nullptr) {
        continue;
      }

      switch (record->status) {
        case BGPSTREAM_RECORD_STATUS_VALID_RECORD:
          break;
        case BGPSTREAM_RECORD_STATUS_FILTERED_SOURCE:
        case BGPSTREAM_RECORD_STATUS_EMPTY_SOURCE:
        case BGPSTREAM_RECORD_STATUS_OUTSIDE_TIME_INTERVAL:
          continue;
        default:
          throw std::runtime_error(record_status_to_string(record->status));
      }

      bgpstream_elem_t *elem = nullptr;
      while (true) {
        const int elem_rc = bgpstream_record_get_next_elem(record, &elem);
        if (elem_rc == 0) {
          break;
        }
        if (elem_rc < 0) {
          throw std::runtime_error("Failed to read BGP element from record");
        }
        if (elem == nullptr) {
          continue;
        }
        if (elem->type != BGPSTREAM_ELEM_TYPE_ANNOUNCEMENT) {
          continue;
        }
        if (!(start_epoch <= static_cast<std::time_t>(record->time_sec) &&
              static_cast<std::time_t>(record->time_sec) < end_exclusive_epoch)) {
          continue;
        }

        stats.scanned_update_count += 1;

        if (elem->as_path == nullptr) {
          continue;
        }

        const std::string prefix = prefix_to_string(elem->prefix);
        stats.usable_update_count += 1;
        add_asns_from_path(elem->as_path, &stats.prefix_to_ases[prefix]);
      }
    }

    destroy_stream();
    return stats;
  } catch (...) {
    destroy_stream();
    throw;
  }
}

void merge_file_stats_into_concurrent_aggregate(ConcurrentAggregate *aggregate,
                                                FileStats &&file_stats) {
  aggregate->scanned_update_count.fetch_add(
      file_stats.scanned_update_count, std::memory_order_relaxed);
  aggregate->usable_update_count.fetch_add(
      file_stats.usable_update_count, std::memory_order_relaxed);

  for (auto &entry : file_stats.prefix_to_ases) {
    const std::size_t shard_index =
        std::hash<std::string>{}(entry.first) % aggregate->shards.size();
    AggregateShard &shard = aggregate->shards[shard_index];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto &merged = shard.prefix_to_ases[entry.first];
    merged.insert(entry.second.begin(), entry.second.end());
  }
}

StatsSummary finalize_concurrent_aggregate(ConcurrentAggregate &&aggregate) {
  StatsSummary summary;
  summary.scanned_update_count =
      aggregate.scanned_update_count.load(std::memory_order_relaxed);
  summary.usable_update_count =
      aggregate.usable_update_count.load(std::memory_order_relaxed);
  summary.skipped_parse_files =
      aggregate.skipped_parse_files.load(std::memory_order_relaxed);

  for (AggregateShard &shard : aggregate.shards) {
    summary.unique_prefixes += shard.prefix_to_ases.size();
    for (const auto &entry : shard.prefix_to_ases) {
      summary.prefix_scoped_as_total += entry.second.size();
    }
  }

  return summary;
}

StatsSummary collect_prefix_stats(const std::vector<fs::path> &files,
                                  std::time_t start_epoch,
                                  std::time_t end_exclusive_epoch,
                                  int stats_workers,
                                  int stats_batch_size) {
  ConcurrentAggregate aggregate;
  StatsProgressDisplay progress_display(files.size(), total_file_bytes(files));
  const std::size_t worker_count = std::min<std::size_t>(files.size(), stats_workers);

  try {
    std::atomic<std::size_t> next_file_index{0};
    std::mutex parse_failures_mutex;
    std::mutex fatal_error_mutex;
    std::exception_ptr fatal_error;
    std::vector<std::string> parse_failures;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
      (void)worker_index;
      workers.emplace_back([&]() {
        try {
          while (true) {
            const std::size_t batch_start =
                next_file_index.fetch_add(stats_batch_size, std::memory_order_relaxed);
            if (batch_start >= files.size()) {
              break;
            }

            const std::size_t batch_end =
                std::min(batch_start + static_cast<std::size_t>(stats_batch_size), files.size());
            std::size_t batch_completed_files = 0;
            std::uint64_t batch_completed_bytes = 0;

            try {
              for (std::size_t file_index = batch_start; file_index < batch_end; ++file_index) {
                const fs::path &file_path = files[file_index];
                std::optional<FileStats> file_stats;
                try {
                  file_stats = collect_file_prefix_stats(
                      file_path, start_epoch, end_exclusive_epoch);
                } catch (const std::exception &exc) {
                  aggregate.skipped_parse_files.fetch_add(1, std::memory_order_relaxed);
                  std::lock_guard<std::mutex> lock(parse_failures_mutex);
                  parse_failures.push_back(format_parse_failure(file_path, exc.what()));
                }

                if (file_stats.has_value()) {
                  merge_file_stats_into_concurrent_aggregate(
                      &aggregate, std::move(*file_stats));
                }

                batch_completed_files += 1;
                batch_completed_bytes += safe_file_size(file_path);
              }
            } catch (...) {
              if (batch_completed_files > 0) {
                progress_display.mark_batch_completed(
                    batch_completed_files, batch_completed_bytes);
              }
              throw;
            }

            if (batch_completed_files > 0) {
              progress_display.mark_batch_completed(
                  batch_completed_files, batch_completed_bytes);
            }
          }
        } catch (...) {
          std::lock_guard<std::mutex> lock(fatal_error_mutex);
          if (fatal_error == nullptr) {
            fatal_error = std::current_exception();
          }
        }
      });
    }

    for (std::thread &worker : workers) {
      worker.join();
    }

    if (fatal_error != nullptr) {
      std::rethrow_exception(fatal_error);
    }

    progress_display.finish();
    for (const std::string &message : parse_failures) {
      std::cerr << message << '\n';
    }
    return finalize_concurrent_aggregate(std::move(aggregate));
  } catch (...) {
    progress_display.close();
    throw;
  }
}

std::vector<fs::path> filter_existing_files(const std::vector<fs::path> &files) {
  std::vector<fs::path> existing;
  existing.reserve(files.size());

  for (const fs::path &file_path : files) {
    if (fs::exists(file_path)) {
      existing.push_back(file_path);
    }
  }
  return existing;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Config config = parse_args(argc, argv);
    const ClosedDateRange range = parse_closed_date_range(config);
    const fs::path data_dir = config.output_dir / config.project / config.collector / "updates";

    fs::create_directories(data_dir);

    std::cout << "download phase" << std::endl;
    const std::vector<fs::path> target_files = collect_target_files(config, range);

    const int download_exit_code =
        run_streaming_command(build_download_command(config, range, false));
    if (download_exit_code != 0) {
      throw std::runtime_error("download.py failed with exit code " +
                               std::to_string(download_exit_code));
    }

    const std::vector<fs::path> existing_files = filter_existing_files(target_files);
    if (existing_files.empty()) {
      throw std::runtime_error("No local files available for statistics.");
    }

    std::cout << "stats phase" << std::endl;
    const StatsSummary stats = collect_prefix_stats(
        existing_files,
        range.start_epoch,
        range.end_exclusive_epoch,
        config.stats_workers,
        config.stats_batch_size);

    std::cout << "start_date: " << config.start_date << '\n';
    std::cout << "end_date: " << config.end_date << '\n';
    std::cout << "collector: " << config.collector << '\n';
    std::cout << "data_dir: " << fs::absolute(data_dir).string() << '\n';
    std::cout << "download_workers: " << config.download_workers << '\n';
    std::cout << "stats_workers: " << config.stats_workers << '\n';
    std::cout << "stats_batch_size: " << config.stats_batch_size << '\n';
    std::cout << "files_used: " << existing_files.size() << '\n';
    std::cout << "scanned_update_elements: " << stats.scanned_update_count << '\n';
    std::cout << "usable_update_elements: " << stats.usable_update_count << '\n';
    std::cout << "skipped_parse_files: " << stats.skipped_parse_files << '\n';
    std::cout << "unique_prefixes: " << stats.unique_prefixes << '\n';
    std::cout << "prefix_scoped_as_total: " << stats.prefix_scoped_as_total << '\n';
    return 0;
  } catch (const std::exception &exc) {
    std::cerr << exc.what() << '\n';
    return 1;
  }
}
