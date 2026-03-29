#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace bgpstream_runner {

inline constexpr char kDefaultStartDate[] = "2025-01-01";
inline constexpr char kDefaultEndDate[] = "2026-01-01";
inline constexpr char kDefaultProject[] = "routeviews";
inline constexpr char kDefaultCollector[] = "route-views.sg";
inline constexpr char kDefaultDataRoot[] = "bgpdata";
inline constexpr char kDefaultConfigPath[] = "config.json";
inline constexpr char kDefaultProcessorPlugin[] = "";
inline constexpr int kDefaultDownloadWorkers = 32;
inline constexpr int kDefaultParserWorkers = 8;
inline constexpr int kDefaultMessageBatchSize = 4096;
inline constexpr int kDefaultChunkSize = 1;
inline constexpr double kDefaultMaxCacheSizeGiB = 10.0;

enum class ChunkUnit {
    Day,
    Month,
};

struct Config {
    std::string start_date = kDefaultStartDate;
    std::string end_date = kDefaultEndDate;
    std::string project = kDefaultProject;
    std::string collector = kDefaultCollector;
    std::string processor_plugin = kDefaultProcessorPlugin;
    std::filesystem::path output_dir = kDefaultDataRoot;
    int download_workers = kDefaultDownloadWorkers;
    int parser_workers = kDefaultParserWorkers;
    int message_batch_size = kDefaultMessageBatchSize;
    int chunk_size = kDefaultChunkSize;
    ChunkUnit chunk_unit = ChunkUnit::Month;
    double max_cache_size_gb = kDefaultMaxCacheSizeGiB;
    int limit = -1;
    bool log_phase_transitions = true;
    bool log_chunk_summary = true;
    bool log_final_summary = true;
};

struct ClosedDateRange {
    std::time_t start_epoch{};
    std::time_t end_exclusive_epoch{};
};

struct RangeProcessingStats {
    std::size_t files_used = 0;
    std::size_t chunk_count = 0;
    std::uint64_t visited_messages = 0;
    std::uint64_t announcement_messages = 0;
    std::uint64_t withdrawal_messages = 0;
    std::uint64_t skipped_parse_files = 0;
};

enum class BGPMessageType {
    Announcement,
    Withdrawal,
};

struct BGPMessage {
    BGPMessageType type = BGPMessageType::Announcement;
    std::time_t timestamp{};
    std::string prefix;
    std::vector<std::uint32_t> asns;
};

}  // namespace bgpstream_runner
