#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <ostream>
#include <string_view>
#include <utility>
#include <vector>

#include "bgpstream_runner/download_client.h"
#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/types.h"

namespace bgpstream_runner {

class ChunkEngine {
   public:
    ChunkEngine(Config config, MessageProcessor &processor);

    RangeProcessingStats run();
    RangeProcessingStats current_stats() const;
    void print_summary(std::ostream &out, const RangeProcessingStats &stats, std::string_view title) const;
    std::filesystem::path write_record_file(const RangeProcessingStats &stats, std::string_view title,
                                            std::string_view run_status, std::string_view error_message = {}) const;

   private:
    struct FileTraversalStats {
        std::uint64_t visited_messages = 0;
        std::uint64_t rib_messages = 0;
        std::uint64_t announcement_messages = 0;
        std::uint64_t withdrawal_messages = 0;
        std::uint64_t peer_state_messages = 0;
        std::uint64_t end_of_rib_messages = 0;
    };

    struct PlannedDownloadEstimate {
        std::uint64_t additional_bytes = 0;
        bool all_sizes_known = true;
    };

    using MessageTimestamp = std::pair<std::time_t, std::uint32_t>;

    void process_files(const std::vector<std::filesystem::path> &files, const ClosedDateRange &chunk);
    FileTraversalStats traverse_single_file(const std::filesystem::path &file_path, const ClosedDateRange &chunk,
                                            std::mutex *processor_mutex);
    void dispatch_message_batch(std::vector<BGPMessage> &messages, std::mutex *processor_mutex);
    void reset_stats();
    void increment_chunk_count();
    void record_processed_file(const FileTraversalStats &file_stats);
    void record_skipped_parse_file();
    std::vector<std::filesystem::path> existing_target_files(const std::vector<DownloadTarget> &targets) const;
    std::uint64_t cache_size_bytes() const;
    PlannedDownloadEstimate planned_download_bytes(const std::vector<DownloadTarget> &targets) const;
    void evict_cache_if_needed(const std::vector<DownloadTarget> &targets) const;

    Config config_;
    DownloadClient download_client_;
    MessageProcessor &processor_;
    const bool processor_requires_strict_chronological_order_;
    const bool processor_uses_concurrent_message_handling_;
    std::optional<MessageTimestamp> last_delivered_message_timestamp_;
    mutable std::mutex record_file_mutex_;
    std::filesystem::path record_file_path_;
    mutable std::mutex stats_mutex_;
    RangeProcessingStats stats_;
};

}  // namespace bgpstream_runner
