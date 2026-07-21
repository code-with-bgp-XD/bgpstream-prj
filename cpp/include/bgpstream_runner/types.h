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

enum class ChunkUnit {
    Day,
    Month,
};

struct CacheConfig {
    std::string start_date = kDefaultStartDate;
    std::string end_date = kDefaultEndDate;
    std::string project = kDefaultProject;
    std::string collector = kDefaultCollector;
    std::filesystem::path output_dir = kDefaultDataRoot;
    int download_workers = kDefaultDownloadWorkers;
    int parser_workers = kDefaultParserWorkers;
    int message_batch_size = kDefaultMessageBatchSize;
    int limit = -1;
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
    bool parse_on_cache_miss = false;
    bool persist_realtime_parsed_cache = false;
    int chunk_size = kDefaultChunkSize;
    ChunkUnit chunk_unit = ChunkUnit::Month;
    int limit = -1;
    bool log_phase_transitions = true;
    bool log_chunk_summary = true;
    bool log_final_summary = true;
    bool download_only = false;
    CacheConfig cache;
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
    std::uint64_t realtime_parsed_files = 0;
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

// Plugins declare the BGPMessage data they consume with this bit mask. Fields
// not requested by the active plugin are left at their default value instead
// of being converted or copied from libBGPStream.
enum class BGPMessageFields : std::uint64_t {
    None = 0,
    Type = std::uint64_t{1} << 0,
    RecordType = std::uint64_t{1} << 1,
    RecordStatus = std::uint64_t{1} << 2,
    Timestamp = std::uint64_t{1} << 3,
    ProjectName = std::uint64_t{1} << 4,
    CollectorName = std::uint64_t{1} << 5,
    RouterName = std::uint64_t{1} << 6,
    RouterIp = std::uint64_t{1} << 7,
    DumpPosition = std::uint64_t{1} << 8,
    DumpTimestamp = std::uint64_t{1} << 9,
    SourceFile = std::uint64_t{1} << 10,
    RecordIndex = std::uint64_t{1} << 11,
    ElementIndex = std::uint64_t{1} << 12,
    OriginatedTimestamp = std::uint64_t{1} << 13,
    PeerIp = std::uint64_t{1} << 14,
    PeerAsn = std::uint64_t{1} << 15,
    Prefix = std::uint64_t{1} << 16,
    NextHop = std::uint64_t{1} << 17,
    HasASPath = std::uint64_t{1} << 18,
    ASPathString = std::uint64_t{1} << 19,
    ASPathSegments = std::uint64_t{1} << 20,
    FlattenedAsns = std::uint64_t{1} << 21,
    OriginAsn = std::uint64_t{1} << 22,
    HasCommunities = std::uint64_t{1} << 23,
    Communities = std::uint64_t{1} << 24,
    Origin = std::uint64_t{1} << 25,
    Med = std::uint64_t{1} << 26,
    LocalPref = std::uint64_t{1} << 27,
    AtomicAggregate = std::uint64_t{1} << 28,
    Aggregator = std::uint64_t{1} << 29,
    PeerStates = std::uint64_t{1} << 30,
    Annotations = std::uint64_t{1} << 31,
};

inline constexpr BGPMessageFields kAllBGPMessageFields =
    static_cast<BGPMessageFields>((static_cast<std::uint64_t>(BGPMessageFields::Annotations) << 1) - 1);

constexpr BGPMessageFields operator|(BGPMessageFields left, BGPMessageFields right) noexcept {
    return static_cast<BGPMessageFields>(static_cast<std::uint64_t>(left) |
                                         static_cast<std::uint64_t>(right));
}

constexpr BGPMessageFields &operator|=(BGPMessageFields &left, BGPMessageFields right) noexcept {
    left = left | right;
    return left;
}

constexpr bool has_message_field(BGPMessageFields fields, BGPMessageFields field) noexcept {
    const auto field_bits = static_cast<std::uint64_t>(field);
    return (static_cast<std::uint64_t>(fields) & field_bits) == field_bits;
}

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
    BGPMessageType type = BGPMessageType::Unknown;

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

    // Independent string, structured, flattened, and origin views of the AS
    // path. Only the views requested by the processor are populated.
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
