extern "C" {
#include <bgpstream.h>
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
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

namespace fs = std::filesystem;

namespace {

constexpr const char *kStartDate = "2025-11-01";
constexpr const char *kEndDate = "2025-12-01";
constexpr const char *kProject = "routeviews";
constexpr const char *kCollector = "route-views.sg";
constexpr const char *kDataRoot = "bgpdata";
constexpr int kDownloadWorkers = 32;

struct Config {
  std::string start_date = kStartDate;
  std::string end_date = kEndDate;
  std::string project = kProject;
  std::string collector = kCollector;
  fs::path output_dir = kDataRoot;
  int download_workers = kDownloadWorkers;
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

struct AggregateStats {
  std::unordered_map<std::string, std::unordered_set<uint32_t>> prefix_to_ases;
  std::uint64_t scanned_update_count = 0;
  std::uint64_t usable_update_count = 0;
  std::uint64_t skipped_parse_files = 0;
};

struct StatsProgressTracker {
  std::size_t total_files = 0;
  std::uint64_t total_bytes = 0;
  std::atomic<std::size_t> completed_files{0};
  std::atomic<std::uint64_t> completed_bytes{0};
  std::atomic<bool> stop_requested{false};
  std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
  mutable std::mutex state_mutex;
  std::size_t active_file_index = 0;
  std::string active_file_name;
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

void initialize_stats_progress_tracker(StatsProgressTracker *tracker,
                                       const std::vector<fs::path> &files) {
  tracker->total_files = files.size();
  tracker->total_bytes = 0;
  tracker->started_at = std::chrono::steady_clock::now();
  for (const fs::path &file_path : files) {
    tracker->total_bytes += safe_file_size(file_path);
  }
}

void stats_tracker_set_active_file(StatsProgressTracker *tracker,
                                   std::size_t file_index,
                                   const fs::path &file_path) {
  std::lock_guard<std::mutex> lock(tracker->state_mutex);
  tracker->active_file_index = file_index + 1;
  tracker->active_file_name = file_path.filename().string();
}

void stats_tracker_mark_file_finished(StatsProgressTracker *tracker,
                                      const fs::path &file_path) {
  tracker->completed_files.fetch_add(1, std::memory_order_relaxed);
  tracker->completed_bytes.fetch_add(safe_file_size(file_path), std::memory_order_relaxed);
}

std::string build_stats_progress_line(const StatsProgressTracker &tracker) {
  static constexpr std::size_t kBarWidth = 32;

  const std::size_t completed_files = tracker.completed_files.load(std::memory_order_relaxed);
  const std::uint64_t completed_bytes = tracker.completed_bytes.load(std::memory_order_relaxed);
  const double fraction =
      tracker.total_files == 0 ? 1.0 : static_cast<double>(completed_files) / tracker.total_files;
  const auto filled =
      static_cast<std::size_t>(fraction * static_cast<double>(kBarWidth));
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - tracker.started_at);

  std::size_t active_file_index = 0;
  std::string active_file_name;
  {
    std::lock_guard<std::mutex> lock(tracker.state_mutex);
    active_file_index = tracker.active_file_index;
    active_file_name = tracker.active_file_name;
  }

  std::ostringstream output;
  output << "stats [";
  for (std::size_t index = 0; index < kBarWidth; ++index) {
    output << (index < filled ? '#' : '-');
  }
  output << "] " << std::fixed << std::setprecision(1) << (fraction * 100.0) << "% "
         << completed_files << "/" << tracker.total_files << " files "
         << format_bytes(completed_bytes) << "/" << format_bytes(tracker.total_bytes)
         << " elapsed=" << format_elapsed(elapsed);

  if (!active_file_name.empty() && completed_files < tracker.total_files) {
    output << " current=" << active_file_index << "/" << tracker.total_files
           << " " << active_file_name;
  }

  return output.str();
}

void drive_stats_progress(const StatsProgressTracker *tracker) {
  const bool is_tty = ::isatty(STDOUT_FILENO) == 1;
  auto last_non_tty_emit = std::chrono::steady_clock::now() - std::chrono::seconds(10);

  while (!tracker->stop_requested.load(std::memory_order_relaxed)) {
    const std::string line = build_stats_progress_line(*tracker);
    if (is_tty) {
      std::cout << '\r' << line << std::string(8, ' ') << std::flush;
    } else {
      const auto now = std::chrono::steady_clock::now();
      if (now - last_non_tty_emit >= std::chrono::seconds(5)) {
        std::cout << line << '\n' << std::flush;
        last_non_tty_emit = now;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  const std::string final_line = build_stats_progress_line(*tracker);
  if (is_tty) {
    std::cout << '\r' << final_line << std::string(8, ' ') << '\n';
  } else {
    std::cout << final_line << '\n';
  }
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

AggregateStats collect_prefix_stats(const std::vector<fs::path> &files,
                                    std::time_t start_epoch,
                                    std::time_t end_exclusive_epoch) {
  AggregateStats aggregate;
  StatsProgressTracker tracker;
  initialize_stats_progress_tracker(&tracker, files);
  std::thread progress_thread(drive_stats_progress, &tracker);

  try {
    const std::size_t total_files = files.size();

    for (std::size_t index = 0; index < total_files; ++index) {
      const fs::path &file_path = files[index];
      stats_tracker_set_active_file(&tracker, index, file_path);

      try {
        FileStats file_stats =
            collect_file_prefix_stats(file_path, start_epoch, end_exclusive_epoch);
        aggregate.scanned_update_count += file_stats.scanned_update_count;
        aggregate.usable_update_count += file_stats.usable_update_count;

        // Keep file-local accumulation until the file fully succeeds, matching the
        // all-or-nothing merge behavior in the original Python code.
        for (auto &entry : file_stats.prefix_to_ases) {
          auto &target = aggregate.prefix_to_ases[entry.first];
          target.insert(entry.second.begin(), entry.second.end());
        }
      } catch (const std::exception &exc) {
        aggregate.skipped_parse_files += 1;
        std::cerr << format_parse_failure(file_path, exc.what()) << '\n';
      }

      stats_tracker_mark_file_finished(&tracker, file_path);
    }

    tracker.stop_requested.store(true, std::memory_order_relaxed);
    progress_thread.join();
    return aggregate;
  } catch (...) {
    tracker.stop_requested.store(true, std::memory_order_relaxed);
    progress_thread.join();
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
    const AggregateStats stats = collect_prefix_stats(
        existing_files, range.start_epoch, range.end_exclusive_epoch);

    std::uint64_t prefix_scoped_as_total = 0;
    for (const auto &entry : stats.prefix_to_ases) {
      prefix_scoped_as_total += entry.second.size();
    }

    std::cout << "start_date: " << config.start_date << '\n';
    std::cout << "end_date: " << config.end_date << '\n';
    std::cout << "collector: " << config.collector << '\n';
    std::cout << "data_dir: " << fs::absolute(data_dir).string() << '\n';
    std::cout << "download_workers: " << config.download_workers << '\n';
    std::cout << "files_used: " << existing_files.size() << '\n';
    std::cout << "scanned_update_elements: " << stats.scanned_update_count << '\n';
    std::cout << "usable_update_elements: " << stats.usable_update_count << '\n';
    std::cout << "skipped_parse_files: " << stats.skipped_parse_files << '\n';
    std::cout << "unique_prefixes: " << stats.prefix_to_ases.size() << '\n';
    std::cout << "prefix_scoped_as_total: " << prefix_scoped_as_total << '\n';
    return 0;
  } catch (const std::exception &exc) {
    std::cerr << exc.what() << '\n';
    return 1;
  }
}
