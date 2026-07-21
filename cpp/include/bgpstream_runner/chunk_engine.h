#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <ostream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bgpstream_runner/download_client.h"
#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/mrt_parser.h"
#include "bgpstream_runner/types.h"

namespace bgpstream_runner {

class FileProgressDisplay;

class ChunkEngine {
   public:
    ChunkEngine(Config config, MessageProcessor &processor);

    RangeProcessingStats run();
    RangeProcessingStats current_stats() const;
    void print_summary(std::ostream &out, const RangeProcessingStats &stats, std::string_view title) const;
    std::filesystem::path write_record_file(const RangeProcessingStats &stats, std::string_view title,
                                            std::string_view run_status, std::string_view error_message = {}) const;

   private:
    using MessageTimestamp = std::pair<std::time_t, std::uint32_t>;
    using SourceFileSet = std::unordered_set<std::filesystem::path>;

    struct FileTraversalResult {
        MessageTraversalStats stats;
        bool used_realtime_parser = false;
    };

    void process_files(const std::vector<std::filesystem::path> &files, const ClosedDateRange &chunk,
                       FileProgressDisplay &progress,
                       const SourceFileSet &force_realtime_parse_files);
    FileTraversalResult traverse_single_file(const std::filesystem::path &file_path,
                                             const ClosedDateRange &chunk,
                                             std::mutex *processor_mutex,
                                             bool force_realtime_parse);
    void dispatch_message_batch(std::vector<BGPMessage> &messages, std::mutex *processor_mutex);
    void reset_stats();
    void increment_chunk_count();
    void record_realtime_parsed_files(std::size_t file_count);
    void record_processed_file(const FileTraversalResult &file_result);

    Config config_;
    DownloadClient download_client_;
    MessageProcessor &processor_;
    const bool processor_requires_strict_chronological_order_;
    const bool processor_uses_concurrent_message_handling_;
    const BGPMessageFields message_fields_;
    std::optional<MessageTimestamp> last_delivered_message_timestamp_;
    mutable std::mutex record_file_mutex_;
    std::filesystem::path record_file_path_;
    mutable std::mutex stats_mutex_;
    RangeProcessingStats stats_;
};

}  // namespace bgpstream_runner
