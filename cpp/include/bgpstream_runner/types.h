#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
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
    std::uint64_t rib_messages = 0;
    std::uint64_t announcement_messages = 0;
    std::uint64_t withdrawal_messages = 0;
    std::uint64_t peer_state_messages = 0;
    std::uint64_t end_of_rib_messages = 0;
    std::uint64_t skipped_parse_files = 0;
};

enum class BGPMessageType {
    Announcement,
    Withdrawal,
    RIB,
    PeerState,
    EndOfRib,
    Unknown,
};

enum class BGPRecordType {
    Update,
    RIB,
    Unknown,
};

enum class BGPRecordStatus {
    Valid,
    FilteredSource,
    EmptySource,
    OutsideTimeInterval,
    CorruptedSource,
    CorruptedRecord,
    UnsupportedRecord,
    Unknown,
};

enum class BGPDumpPosition {
    Start,
    Middle,
    End,
    Unknown,
};

enum class ASPathSegmentType {
    ASN,
    Set,
    ConfederationSequence,
    ConfederationSet,
    Unknown,
};

struct ASPathSegment {
    ASPathSegmentType type = ASPathSegmentType::Unknown;
    std::vector<std::uint32_t> asns;
};

struct BGPCommunity {
    std::uint16_t asn = 0;
    std::uint16_t value = 0;
};

enum class BGPOrigin {
    IGP,
    EGP,
    Incomplete,
    Unknown,
};

enum class BGPPeerState {
    Unknown,
    Idle,
    Connect,
    Active,
    OpenSent,
    OpenConfirm,
    Established,
    Clearing,
    Deleted,
};

struct BGPAggregator {
    std::uint32_t asn = 0;
    std::string address;
};

struct BGPAnnotations {
    bool rpki_active = false;
    bool has_rpki_config = false;
    std::uint32_t timestamp = 0;
};

struct BGPMessage {
    // Element type. Announcement and Withdrawal retain their original enum
    // values so existing source code continues to behave as before.
    BGPMessageType type = BGPMessageType::Announcement;

    // Record-level provenance and timing.
    BGPRecordType record_type = BGPRecordType::Unknown;
    BGPRecordStatus record_status = BGPRecordStatus::Unknown;
    std::time_t timestamp{};
    std::uint32_t timestamp_microseconds = 0;
    std::string project_name;
    std::string collector_name;
    std::string router_name;
    std::string router_ip;
    BGPDumpPosition dump_position = BGPDumpPosition::Unknown;
    std::time_t dump_timestamp{};
    std::string source_file;
    std::uint64_t record_index = 0;
    std::uint64_t element_index = 0;

    // Element-level source and NLRI information.
    std::time_t originated_timestamp{};
    std::uint32_t originated_timestamp_microseconds = 0;
    std::string peer_ip;
    std::uint32_t peer_asn = 0;
    std::string prefix;
    std::string next_hop;

    // Full path representation plus a structured, lossless representation of
    // every segment exposed by libBGPStream. `asns` is kept as the legacy
    // flattened view for existing processors.
    bool has_as_path = false;
    std::string as_path;
    std::vector<ASPathSegment> as_path_segments;
    std::vector<std::uint32_t> asns;
    std::optional<std::uint32_t> origin_asn;

    // Path attributes available from libBGPStream.
    bool has_communities = false;
    std::vector<BGPCommunity> communities;
    std::optional<BGPOrigin> origin;
    std::optional<std::uint32_t> med;
    std::optional<std::uint32_t> local_pref;
    bool atomic_aggregate = false;
    std::optional<BGPAggregator> aggregator;

    // Peer-state elements and optional libBGPStream annotations.
    std::optional<BGPPeerState> old_peer_state;
    std::optional<BGPPeerState> new_peer_state;
    BGPAnnotations annotations;
};

}  // namespace bgpstream_runner
