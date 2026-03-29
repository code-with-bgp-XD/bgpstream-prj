#pragma once

#include "bgpstream_runner/download_client.h"
#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/types.h"

#include <filesystem>
#include <mutex>
#include <ostream>
#include <string_view>
#include <vector>

namespace bgpstream_runner {

class ChunkEngine {
 public:
  ChunkEngine(Config config, MessageProcessor &processor);

  RangeProcessingStats run();
  RangeProcessingStats current_stats() const;
  void print_summary(
      std::ostream &out,
      const RangeProcessingStats &stats,
      std::string_view title) const;
  std::filesystem::path write_record_file(
      const RangeProcessingStats &stats,
      std::string_view title,
      std::string_view run_status,
      std::string_view error_message = {}) const;

 private:
  struct FileTraversalStats {
    std::uint64_t visited_messages = 0;
    std::uint64_t announcement_messages = 0;
    std::uint64_t withdrawal_messages = 0;
  };

  void process_files(
      const std::vector<std::filesystem::path> &files,
      const ClosedDateRange &chunk);
  FileTraversalStats traverse_single_file(
      const std::filesystem::path &file_path,
      const ClosedDateRange &chunk,
      std::mutex *processor_mutex);
  void cleanup_chunk_files(
      const std::vector<std::filesystem::path> &target_files) const;
  void reset_stats();
  void increment_chunk_count();
  void record_processed_file(const FileTraversalStats &file_stats);
  void record_skipped_parse_file();

  Config config_;
  DownloadClient download_client_;
  MessageProcessor &processor_;
  mutable std::mutex record_file_mutex_;
  std::filesystem::path record_file_path_;
  mutable std::mutex stats_mutex_;
  RangeProcessingStats stats_;
};

}  // namespace bgpstream_runner
