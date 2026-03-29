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
  void print_summary(
      std::ostream &out,
      const RangeProcessingStats &stats,
      std::string_view title) const;

 private:
  struct FileTraversalStats {
    std::uint64_t visited_messages = 0;
    std::uint64_t announcement_messages = 0;
    std::uint64_t withdrawal_messages = 0;
  };

  void process_files(
      const std::vector<std::filesystem::path> &files,
      const ClosedDateRange &chunk,
      RangeProcessingStats *stats);
  FileTraversalStats traverse_single_file(
      const std::filesystem::path &file_path,
      const ClosedDateRange &chunk,
      std::mutex *processor_mutex);
  void cleanup_chunk_files(
      const std::vector<std::filesystem::path> &target_files) const;

  Config config_;
  DownloadClient download_client_;
  MessageProcessor &processor_;
};

}  // namespace bgpstream_runner
