#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "bgpstream_runner/types.h"

namespace bgpstream_runner {

struct CommandResult {
    int exit_code = 0;
    std::string output;
};

class FileProgressDisplay {
   public:
    FileProgressDisplay(std::size_t total_files, std::uint64_t total_bytes);
    FileProgressDisplay(const FileProgressDisplay &) = delete;
    FileProgressDisplay &operator=(const FileProgressDisplay &) = delete;
    ~FileProgressDisplay();

    void mark_batch_completed(std::size_t completed_files, std::uint64_t completed_bytes);
    void finish();
    void close();

   private:
    std::string build_line_locked() const;
    void render_locked();
    void close_locked();

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

[[noreturn]] void print_usage_and_exit(const char *program, int exit_code);
Config parse_args(int argc, char **argv);

std::string trim(std::string value);
std::string format_bytes(std::uint64_t num_bytes);
std::string format_elapsed(std::chrono::seconds elapsed);
std::uint64_t safe_file_size(const std::filesystem::path &file_path);
std::uint64_t total_file_bytes(const std::vector<std::filesystem::path> &files);
std::string shell_escape(std::string_view value);
CommandResult run_capture_command(const std::string &command);
int run_streaming_command(const std::string &command);
ClosedDateRange parse_closed_date_range(const Config &config);
std::string format_utc_timestamp(std::time_t epoch);
std::vector<ClosedDateRange> split_range_by_chunks(const ClosedDateRange &range, int chunk_size, ChunkUnit chunk_unit);
std::string_view chunk_unit_to_string(ChunkUnit chunk_unit);
std::string format_range_label(const ClosedDateRange &range);

}  // namespace bgpstream_runner
