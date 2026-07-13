#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"

namespace {

using bgpstream_runner::ASPathSegment;
using bgpstream_runner::ASPathSegmentType;
using bgpstream_runner::BGPDumpPosition;
using bgpstream_runner::BGPMessage;
using bgpstream_runner::BGPMessageType;
using bgpstream_runner::BGPOrigin;
using bgpstream_runner::BGPPeerState;

constexpr double kLn2 = 0.693147180559945309417232121458176568;

std::string env_text(const char *name) {
    const char *value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

std::uint64_t parse_u64_env(const char *name, std::uint64_t default_value) {
    const std::string text = env_text(name);
    if (text.empty()) {
        return default_value;
    }
    std::size_t used = 0;
    const unsigned long long value = std::stoull(text, &used);
    if (used != text.size()) {
        throw std::invalid_argument(std::string("Invalid integer in ") + name + ": " + text);
    }
    return static_cast<std::uint64_t>(value);
}

double parse_double_env(const char *name, double default_value) {
    const std::string text = env_text(name);
    if (text.empty()) {
        return default_value;
    }
    std::size_t used = 0;
    const double value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value)) {
        throw std::invalid_argument(std::string("Invalid number in ") + name + ": " + text);
    }
    return value;
}

bool parse_bool_env(const char *name, bool default_value) {
    std::string text = env_text(name);
    if (text.empty()) {
        return default_value;
    }
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        return false;
    }
    throw std::invalid_argument(std::string("Invalid boolean in ") + name + ": " + text);
}

struct RuntimeConfig {
    std::size_t top_k = 20;
    double half_life_seconds = 3600.0;
    std::uint64_t warmup_seconds = 0;
    std::uint64_t session_sync_grace_seconds = 300;
    double minimum_origin_route_hours = 1000.0;
    bool strict_single_router = true;
    bool assume_complete_at_start = false;

    static RuntimeConfig from_environment() {
        RuntimeConfig config;
        config.top_k = static_cast<std::size_t>(parse_u64_env("BGP_CHURN_TOP_K", config.top_k));
        config.half_life_seconds =
            parse_double_env("BGP_CHURN_HALF_LIFE_SECONDS", config.half_life_seconds);
        config.warmup_seconds = parse_u64_env("BGP_CHURN_WARMUP_SECONDS", config.warmup_seconds);
        config.session_sync_grace_seconds =
            parse_u64_env("BGP_CHURN_SESSION_SYNC_GRACE_SECONDS", config.session_sync_grace_seconds);
        config.minimum_origin_route_hours =
            parse_double_env("BGP_CHURN_MIN_ORIGIN_ROUTE_HOURS", config.minimum_origin_route_hours);
        config.strict_single_router =
            parse_bool_env("BGP_CHURN_STRICT_SINGLE_ROUTER", config.strict_single_router);
        config.assume_complete_at_start =
            parse_bool_env("BGP_CHURN_ASSUME_COMPLETE_AT_START", config.assume_complete_at_start);

        if (config.top_k == 0) {
            throw std::invalid_argument("BGP_CHURN_TOP_K must be greater than zero");
        }
        if (!(config.half_life_seconds > 0.0)) {
            throw std::invalid_argument("BGP_CHURN_HALF_LIFE_SECONDS must be greater than zero");
        }
        if (config.minimum_origin_route_hours < 0.0) {
            throw std::invalid_argument("BGP_CHURN_MIN_ORIGIN_ROUTE_HOURS must not be negative");
        }
        return config;
    }
};

struct EventTime {
    std::int64_t seconds = 0;
    std::uint32_t microseconds = 0;
};

bool operator<(const EventTime &lhs, const EventTime &rhs) {
    return std::tie(lhs.seconds, lhs.microseconds) < std::tie(rhs.seconds, rhs.microseconds);
}

bool operator<=(const EventTime &lhs, const EventTime &rhs) { return !(rhs < lhs); }

EventTime message_time(const BGPMessage &message) {
    return EventTime{static_cast<std::int64_t>(message.timestamp), message.timestamp_microseconds};
}

EventTime add_seconds(EventTime value, std::uint64_t seconds) {
    const auto max_value = std::numeric_limits<std::int64_t>::max();
    if (seconds > static_cast<std::uint64_t>(max_value) ||
        value.seconds > max_value - static_cast<std::int64_t>(seconds)) {
        value.seconds = max_value;
    } else {
        value.seconds += static_cast<std::int64_t>(seconds);
    }
    return value;
}

double elapsed_seconds(const EventTime &start, const EventTime &end) {
    if (end < start) {
        return 0.0;
    }
    const double seconds = static_cast<double>(end.seconds - start.seconds);
    const double micros = static_cast<double>(static_cast<std::int64_t>(end.microseconds) -
                                              static_cast<std::int64_t>(start.microseconds)) /
                          1'000'000.0;
    return std::max(0.0, seconds + micros);
}

struct OrderKey {
    EventTime time;
    std::string source_file;
    std::uint64_t record_index = 0;
    std::uint64_t element_index = 0;
};

bool operator<(const OrderKey &lhs, const OrderKey &rhs) {
    return std::tie(lhs.time.seconds, lhs.time.microseconds, lhs.source_file, lhs.record_index, lhs.element_index) <
           std::tie(rhs.time.seconds, rhs.time.microseconds, rhs.source_file, rhs.record_index, rhs.element_index);
}

OrderKey message_order_key(const BGPMessage &message) {
    return OrderKey{message_time(message), message.source_file, message.record_index, message.element_index};
}

struct DecayedCounter {
    double value = 0.0;
    EventTime last_time{};
    bool initialized = false;

    void add(const EventTime &time, double weight, double lambda) {
        if (!(weight > 0.0)) {
            return;
        }
        if (!initialized) {
            value = weight;
            last_time = time;
            initialized = true;
            return;
        }
        if (last_time < time) {
            value *= std::exp(-lambda * elapsed_seconds(last_time, time));
            last_time = time;
        }
        // For a late event, preserve monotonic time and place its mass at the
        // latest known instant. The caller separately records ordering errors.
        value += weight;
    }

    double value_at(const EventTime &time, double lambda) const {
        if (!initialized) {
            return 0.0;
        }
        if (time <= last_time) {
            return value;
        }
        return value * std::exp(-lambda * elapsed_seconds(last_time, time));
    }

    double rate_per_hour(const EventTime &time, double lambda) const {
        return 3600.0 * lambda * value_at(time, lambda);
    }
};

struct Hash128 {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
};

bool operator==(const Hash128 &lhs, const Hash128 &rhs) {
    return lhs.first == rhs.first && lhs.second == rhs.second;
}

bool operator!=(const Hash128 &lhs, const Hash128 &rhs) { return !(lhs == rhs); }

class DualFNV1a {
   public:
    DualFNV1a() = default;

    void add_byte(std::uint8_t value) {
        first_ ^= value;
        first_ *= 1099511628211ULL;

        second_ ^= static_cast<std::uint8_t>(value + 0x9dU);
        second_ *= 14029467366897019727ULL;
        second_ ^= second_ >> 29U;
    }

    void add_bool(bool value) { add_byte(static_cast<std::uint8_t>(value ? 1U : 0U)); }

    template <typename Integer>
    void add_integer(Integer value) {
        using Unsigned = std::make_unsigned_t<Integer>;
        Unsigned converted = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
            add_byte(static_cast<std::uint8_t>((converted >> (index * 8U)) & static_cast<Unsigned>(0xffU)));
        }
    }

    void add_string(std::string_view text) {
        add_integer<std::uint64_t>(static_cast<std::uint64_t>(text.size()));
        for (unsigned char ch : text) {
            add_byte(ch);
        }
    }

    Hash128 finish() const { return Hash128{first_, second_}; }

   private:
    std::uint64_t first_ = 1469598103934665603ULL;
    std::uint64_t second_ = 1099511628211ULL ^ 0xd6e8feb86659fd93ULL;
};

Hash128 hash_string(std::string_view value) {
    DualFNV1a hasher;
    hasher.add_string(value);
    return hasher.finish();
}

bool is_set_segment(ASPathSegmentType type) {
    return type == ASPathSegmentType::Set || type == ASPathSegmentType::ConfederationSet;
}

bool is_confederation_segment(ASPathSegmentType type) {
    return type == ASPathSegmentType::ConfederationSequence || type == ASPathSegmentType::ConfederationSet;
}

std::vector<std::uint32_t> canonical_segment_asns(const ASPathSegment &segment) {
    std::vector<std::uint32_t> values = segment.asns;
    if (is_set_segment(segment.type)) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }
    return values;
}

std::vector<std::uint32_t> derive_origin_set(const BGPMessage &message) {
    for (auto iterator = message.as_path_segments.rbegin(); iterator != message.as_path_segments.rend(); ++iterator) {
        if (is_confederation_segment(iterator->type)) {
            continue;
        }
        if (iterator->type == ASPathSegmentType::ASN && !iterator->asns.empty()) {
            return std::vector<std::uint32_t>{iterator->asns.back()};
        }
        if (iterator->type == ASPathSegmentType::Set && !iterator->asns.empty()) {
            std::vector<std::uint32_t> origins = iterator->asns;
            std::sort(origins.begin(), origins.end());
            origins.erase(std::unique(origins.begin(), origins.end()), origins.end());
            return origins;
        }
    }

    if (message.origin_asn.has_value()) {
        return std::vector<std::uint32_t>{*message.origin_asn};
    }
    if (!message.asns.empty()) {
        return std::vector<std::uint32_t>{message.asns.back()};
    }
    return {};
}

std::vector<std::uint32_t> derive_path_member_set(const BGPMessage &message) {
    std::vector<std::uint32_t> members;
    if (!message.as_path_segments.empty()) {
        for (const ASPathSegment &segment : message.as_path_segments) {
            if (is_confederation_segment(segment.type)) {
                continue;
            }
            members.insert(members.end(), segment.asns.begin(), segment.asns.end());
        }
    } else {
        // Compatibility fallback. Without segment metadata confederation ASNs
        // cannot be excluded, so the legacy flattened representation is used.
        members = message.asns;
    }
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    return members;
}

Hash128 hash_as_path(const BGPMessage &message) {
    DualFNV1a hasher;
    hasher.add_bool(message.has_as_path);
    if (!message.has_as_path) {
        return hasher.finish();
    }

    if (!message.as_path_segments.empty()) {
        hasher.add_integer<std::uint64_t>(message.as_path_segments.size());
        for (const ASPathSegment &segment : message.as_path_segments) {
            hasher.add_integer<std::uint8_t>(static_cast<std::uint8_t>(segment.type));
            const std::vector<std::uint32_t> values = canonical_segment_asns(segment);
            hasher.add_integer<std::uint64_t>(values.size());
            for (const std::uint32_t asn : values) {
                hasher.add_integer(asn);
            }
        }
    } else {
        // Compatibility fallback for producers that set has_as_path but do
        // not populate the structured representation.
        hasher.add_string(message.as_path);
    }
    return hasher.finish();
}

Hash128 hash_communities(const BGPMessage &message) {
    DualFNV1a hasher;
    hasher.add_bool(message.has_communities);
    if (!message.has_communities) {
        return hasher.finish();
    }

    std::vector<std::uint32_t> values;
    values.reserve(message.communities.size());
    for (const auto &community : message.communities) {
        values.push_back((static_cast<std::uint32_t>(community.asn) << 16U) |
                         static_cast<std::uint32_t>(community.value));
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());

    hasher.add_integer<std::uint64_t>(values.size());
    for (const std::uint32_t value : values) {
        hasher.add_integer(value);
    }
    return hasher.finish();
}

struct AggregatorDigest {
    bool present = false;
    std::uint32_t asn = 0;
    Hash128 address_hash{};
};

bool operator==(const AggregatorDigest &lhs, const AggregatorDigest &rhs) {
    return lhs.present == rhs.present && lhs.asn == rhs.asn && lhs.address_hash == rhs.address_hash;
}

bool operator!=(const AggregatorDigest &lhs, const AggregatorDigest &rhs) { return !(lhs == rhs); }

struct RouteDigest {
    bool has_as_path = false;
    Hash128 as_path_hash{};
    std::vector<std::uint32_t> origin_set;
    std::vector<std::uint32_t> path_member_set;
    Hash128 next_hop_hash{};
    bool has_communities = false;
    Hash128 communities_hash{};
    std::optional<BGPOrigin> origin;
    std::optional<std::uint32_t> med;
    std::optional<std::uint32_t> local_pref;
    bool atomic_aggregate = false;
    AggregatorDigest aggregator{};
};

RouteDigest make_route_digest(const BGPMessage &message) {
    RouteDigest digest;
    digest.has_as_path = message.has_as_path;
    digest.as_path_hash = hash_as_path(message);
    digest.origin_set = derive_origin_set(message);
    digest.path_member_set = derive_path_member_set(message);
    digest.next_hop_hash = hash_string(message.next_hop);
    digest.has_communities = message.has_communities;
    digest.communities_hash = hash_communities(message);
    digest.origin = message.origin;
    digest.med = message.med;
    digest.local_pref = message.local_pref;
    digest.atomic_aggregate = message.atomic_aggregate;
    if (message.aggregator.has_value()) {
        digest.aggregator.present = true;
        digest.aggregator.asn = message.aggregator->asn;
        digest.aggregator.address_hash = hash_string(message.aggregator->address);
    }
    return digest;
}

enum ChangeBit : std::uint32_t {
    ChangeNone = 0,
    ChangeASPath = 1U << 0U,
    ChangeOriginSet = 1U << 1U,
    ChangeNextHop = 1U << 2U,
    ChangeCommunities = 1U << 3U,
    ChangeOriginAttribute = 1U << 4U,
    ChangeMED = 1U << 5U,
    ChangeLocalPref = 1U << 6U,
    ChangeAtomicAggregate = 1U << 7U,
    ChangeAggregator = 1U << 8U,
};

std::uint32_t route_change_mask(const RouteDigest &old_route, const RouteDigest &new_route) {
    std::uint32_t mask = ChangeNone;
    if (old_route.has_as_path != new_route.has_as_path || old_route.as_path_hash != new_route.as_path_hash) {
        mask |= ChangeASPath;
    }
    if (old_route.origin_set != new_route.origin_set) {
        mask |= ChangeOriginSet;
    }
    if (old_route.next_hop_hash != new_route.next_hop_hash) {
        mask |= ChangeNextHop;
    }
    if (old_route.has_communities != new_route.has_communities ||
        old_route.communities_hash != new_route.communities_hash) {
        mask |= ChangeCommunities;
    }
    if (old_route.origin != new_route.origin) {
        mask |= ChangeOriginAttribute;
    }
    if (old_route.med != new_route.med) {
        mask |= ChangeMED;
    }
    if (old_route.local_pref != new_route.local_pref) {
        mask |= ChangeLocalPref;
    }
    if (old_route.atomic_aggregate != new_route.atomic_aggregate) {
        mask |= ChangeAtomicAggregate;
    }
    if (old_route.aggregator != new_route.aggregator) {
        mask |= ChangeAggregator;
    }
    return mask;
}

struct PrefixStats {
    std::uint64_t raw_total = 0;
    std::uint64_t raw_scored = 0;
    std::uint64_t announcements = 0;
    std::uint64_t withdrawals = 0;
    std::uint64_t rib_baselines = 0;
    std::uint64_t cold_start_announcements = 0;
    std::uint64_t ordinary_effective_changes = 0;
    std::uint64_t new_routes = 0;
    std::uint64_t reannouncements = 0;
    std::uint64_t effective_withdrawals = 0;
    std::uint64_t replacements = 0;
    std::uint64_t duplicate_announcements = 0;
    std::uint64_t duplicate_withdrawals = 0;
    std::uint64_t unknown_withdrawals = 0;
    std::uint64_t path_changes = 0;
    std::uint64_t origin_set_changes = 0;
    std::uint64_t next_hop_changes = 0;
    std::uint64_t community_changes = 0;
    std::uint64_t origin_attribute_changes = 0;
    std::uint64_t med_changes = 0;
    std::uint64_t local_pref_changes = 0;
    std::uint64_t atomic_aggregate_changes = 0;
    std::uint64_t aggregator_changes = 0;
    std::uint64_t session_reset_withdrawals = 0;
    std::uint64_t session_sync_announcements = 0;
    std::uint64_t session_sync_replacements = 0;
    std::uint64_t session_sync_withdrawals = 0;
    std::uint64_t late_rib_resyncs = 0;
    std::uint64_t out_of_order_events = 0;
    DecayedCounter recent_raw;
    DecayedCounter recent_effective;
    DecayedCounter recent_reset;
};

struct PeerStats {
    std::uint64_t update_records_total = 0;
    std::uint64_t update_records_scored = 0;
    std::uint64_t raw_nlri_total = 0;
    std::uint64_t raw_nlri_scored = 0;
    std::uint64_t ordinary_effective_changes = 0;
    std::uint64_t duplicate_announcements = 0;
    std::uint64_t duplicate_withdrawals = 0;
    std::uint64_t unknown_withdrawals = 0;
    std::uint64_t peer_up_events = 0;
    std::uint64_t peer_down_events = 0;
    std::uint64_t session_resets = 0;
    std::uint64_t session_reset_routes = 0;
    std::uint64_t session_sync_announcements = 0;
    std::uint64_t session_sync_replacements = 0;
    std::uint64_t session_sync_withdrawals = 0;
    std::uint64_t end_of_rib_events = 0;
    std::uint64_t out_of_order_events = 0;
    DecayedCounter recent_raw;
    DecayedCounter recent_effective;
};

struct OriginStats {
    double raw_announcement_mass_total = 0.0;
    double raw_announcement_mass_scored = 0.0;
    double ordinary_effective_mass = 0.0;
    double reset_induced_mass = 0.0;
    double new_route_mass = 0.0;
    double reannouncement_mass = 0.0;
    double withdrawal_mass = 0.0;
    double replacement_mass = 0.0;
    double path_change_mass = 0.0;
    double attribute_change_mass = 0.0;
    double origin_change_in_mass = 0.0;
    double origin_change_out_mass = 0.0;
    double route_seconds_closed = 0.0;
    DecayedCounter recent_effective;
    DecayedCounter recent_reset;
};


struct PathAsStats {
    std::uint64_t changed_path_involvement = 0;
    std::uint64_t added_to_path = 0;
    std::uint64_t removed_from_path = 0;
    std::uint64_t retained_in_changed_path = 0;
    std::uint64_t session_induced_involvement = 0;
    DecayedCounter recent_changed_path;
};

struct PeerIdentity {
    std::string router_scope;
    std::string peer_ip;
    std::uint32_t peer_asn = 0;
};

bool operator==(const PeerIdentity &lhs, const PeerIdentity &rhs) {
    return lhs.peer_asn == rhs.peer_asn && lhs.router_scope == rhs.router_scope && lhs.peer_ip == rhs.peer_ip;
}

struct PeerIdentityHash {
    std::size_t operator()(const PeerIdentity &key) const noexcept {
        std::size_t value = std::hash<std::string>{}(key.router_scope);
        value ^= std::hash<std::string>{}(key.peer_ip) + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        value ^= std::hash<std::uint32_t>{}(key.peer_asn) + 0x9e3779b97f4a7c15ULL + (value << 6U) +
                 (value >> 2U);
        return value;
    }
};

struct RouteIdentity {
    std::size_t peer_id = 0;
    std::string prefix;
};

bool operator==(const RouteIdentity &lhs, const RouteIdentity &rhs) {
    return lhs.peer_id == rhs.peer_id && lhs.prefix == rhs.prefix;
}

struct RouteIdentityHash {
    std::size_t operator()(const RouteIdentity &key) const noexcept {
        std::size_t value = std::hash<std::size_t>{}(key.peer_id);
        value ^= std::hash<std::string>{}(key.prefix) + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        return value;
    }
};

struct RouteState {
    std::size_t peer_id = 0;
    std::string prefix;
    bool initialized = false;
    bool active = false;
    RouteDigest route;
    EventTime active_since{};
    OrderKey last_order{};
    bool has_last_order = false;
};

struct PeerData {
    PeerIdentity identity;
    PeerStats stats;
    bool established = false;
    bool saw_peer_state = false;
    bool sync_active = false;
    bool baseline_complete = false;
    bool saw_route_event = false;
    EventTime sync_started{};
    OrderKey last_any_order{};
    bool has_last_any_order = false;
    std::unordered_set<std::size_t> active_route_ids;
};

struct RecordCursor {
    bool initialized = false;
    std::uint64_t current_record_index = 0;
    std::uint64_t maximum_record_index = 0;
};

struct PeerAsAggregate {
    std::uint64_t sessions = 0;
    std::uint64_t update_records_total = 0;
    std::uint64_t update_records_scored = 0;
    std::uint64_t raw_nlri_total = 0;
    std::uint64_t raw_nlri_scored = 0;
    std::uint64_t ordinary_effective_changes = 0;
    std::uint64_t session_resets = 0;
    std::uint64_t session_reset_routes = 0;
    std::uint64_t session_sync_announcements = 0;
    std::uint64_t duplicates = 0;
    double recent_raw_rate = 0.0;
    double recent_effective_rate = 0.0;
};

std::string make_router_scope(const BGPMessage &message) {
    std::ostringstream output;
    output << (message.project_name.empty() ? "<project>" : message.project_name) << '|'
           << (message.collector_name.empty() ? "<collector>" : message.collector_name) << '|';
    if (!message.router_ip.empty()) {
        output << "ip=" << message.router_ip;
    } else if (!message.router_name.empty()) {
        output << "name=" << message.router_name;
    } else {
        output << "collector-router";
    }
    return output.str();
}

PeerIdentity make_peer_identity(const BGPMessage &message, const std::string &router_scope) {
    return PeerIdentity{router_scope, message.peer_ip.empty() ? std::string("<unknown-peer-ip>") : message.peer_ip,
                        message.peer_asn};
}

bool is_established(const std::optional<BGPPeerState> &state) {
    return state.has_value() && *state == BGPPeerState::Established;
}

bool has_non_path_change(std::uint32_t mask) {
    constexpr std::uint32_t kPathBits = ChangeASPath | ChangeOriginSet;
    return (mask & ~kPathBits) != 0U;
}

}  // namespace

class SingleRouterChurnProcessor : public bgpstream_runner::MessageProcessor {
   public:
    SingleRouterChurnProcessor() : config_(RuntimeConfig::from_environment()), lambda_(kLn2 / config_.half_life_seconds) {}

    std::string_view name() const override { return "single_router_churn_v2"; }

    bgpstream_runner::BGPMessageFields required_message_fields() const noexcept override {
        using Fields = bgpstream_runner::BGPMessageFields;
        return Fields::Type | Fields::Timestamp | Fields::ProjectName | Fields::CollectorName |
               Fields::RouterName | Fields::RouterIp | Fields::DumpPosition | Fields::DumpTimestamp |
               Fields::SourceFile | Fields::RecordIndex | Fields::ElementIndex | Fields::PeerIp |
               Fields::PeerAsn | Fields::Prefix | Fields::NextHop | Fields::HasASPath |
               Fields::ASPathString | Fields::ASPathSegments | Fields::FlattenedAsns |
               Fields::OriginAsn | Fields::HasCommunities | Fields::Communities | Fields::Origin |
               Fields::Med | Fields::LocalPref | Fields::AtomicAggregate | Fields::Aggregator |
               Fields::PeerStates;
    }

    bool requires_strict_chronological_order() const noexcept override { return true; }

    void handle_messages(const std::vector<BGPMessage> &messages) override {
        std::vector<const BGPMessage *> ordered;
        ordered.reserve(messages.size());
        for (const BGPMessage &message : messages) {
            ordered.push_back(&message);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const BGPMessage *lhs, const BGPMessage *rhs) {
            return message_order_key(*lhs) < message_order_key(*rhs);
        });

        for (const BGPMessage *message : ordered) {
            process_message(*message);
        }
    }

    void finalize() override { finalized_ = true; }

    void print_summary(std::ostream &out) const override {
        out << std::fixed << std::setprecision(6);
        out << "algorithm: stateful_single_router_visible_attribute_churn\n";
        out << "algorithm_version: 2\n";
        out << "finalized: " << (finalized_ ? "true" : "false") << '\n';
        out << "strict_single_router: " << (config_.strict_single_router ? "true" : "false") << '\n';
        out << "selected_router_scope: "
            << (selected_router_scope_.has_value() ? *selected_router_scope_ : std::string("<none>")) << '\n';
        out << "foreign_router_messages_skipped: " << foreign_router_messages_skipped_ << '\n';
        out << "top_k: " << config_.top_k << '\n';
        out << "half_life_seconds: " << config_.half_life_seconds << '\n';
        out << "warmup_seconds: " << config_.warmup_seconds << '\n';
        out << "session_sync_grace_seconds: " << config_.session_sync_grace_seconds << '\n';
        out << "minimum_origin_route_hours: " << config_.minimum_origin_route_hours << '\n';
        out << "assume_complete_at_start: " << (config_.assume_complete_at_start ? "true" : "false") << '\n';
        out << "observation_started: " << (has_observation_time_ ? observation_start_.seconds : 0) << '\n';
        out << "analysis_started: " << (has_observation_time_ ? analysis_start_.seconds : 0) << '\n';
        out << "observation_ended: " << (has_observation_time_ ? observation_end_.seconds : 0) << '\n';
        out << "observation_seconds: "
            << (has_observation_time_ ? elapsed_seconds(observation_start_, observation_end_) : 0.0) << '\n';
        out << "analysis_seconds: "
            << (has_observation_time_ && analysis_start_ < observation_end_
                    ? elapsed_seconds(analysis_start_, observation_end_)
                    : 0.0)
            << '\n';
        out << "messages_seen: " << messages_seen_ << '\n';
        out << "update_nlri_total: " << raw_update_nlri_total_ << '\n';
        out << "update_nlri_scored: " << raw_update_nlri_scored_ << '\n';
        out << "unique_update_records_total: " << update_records_total_ << '\n';
        out << "unique_update_records_scored: " << update_records_scored_ << '\n';
        out << "rib_elements_seen: " << rib_elements_seen_ << '\n';
        out << "peer_state_elements_seen: " << peer_state_elements_seen_ << '\n';
        out << "end_of_rib_elements_seen: " << end_of_rib_elements_seen_ << '\n';
        out << "ordinary_effective_changes: " << ordinary_effective_changes_ << '\n';
        out << "session_induced_changes: " << reset_induced_changes_ << '\n';
        out << "duplicate_announcements: " << duplicate_announcements_ << '\n';
        out << "duplicate_withdrawals: " << duplicate_withdrawals_ << '\n';
        out << "unknown_withdrawals: " << unknown_withdrawals_ << '\n';
        out << "cold_start_announcements: " << cold_start_announcements_ << '\n';
        out << "unknown_origin_attribution_attempts: " << unknown_origin_events_ << '\n';
        out << "global_time_order_violations: " << global_time_order_violations_ << '\n';
        out << "route_state_order_violations: " << route_state_order_violations_ << '\n';
        out << "peer_state_order_violations: " << peer_state_order_violations_ << '\n';
        out << "record_order_violations: " << record_order_violations_ << '\n';
        out << "distinct_router_scopes_seen: " << observed_router_scopes_.size() << '\n';
        out << "unknown_message_types: " << unknown_message_types_ << '\n';
        out << "peer_cross_prefix_order_observations: " << peer_cross_prefix_order_observations_ << '\n';
        out << "malformed_prefixless_route_events: " << malformed_prefixless_route_events_ << '\n';
        out << "known_peers: " << peers_.size() << '\n';
        out << "baseline_complete_peers: " << baseline_complete_peer_count() << '\n';
        out << "baseline_incomplete_peers: " << (peers_.size() - baseline_complete_peer_count()) << '\n';
        out << "synchronizing_peers: " << synchronizing_peer_count() << '\n';
        out << "known_route_keys: " << route_states_.size() << '\n';
        out << "active_route_keys: " << active_route_count() << '\n';
        out << "unique_prefixes: " << prefix_stats_.size() << '\n';
        out << "unique_origin_asns: " << origin_stats_.size() << '\n';
        out << "unique_path_member_asns: " << path_as_stats_.size() << '\n';
        out << "route_key_scope: router,peer_ip,peer_asn,prefix\n";
        out << "first_seen_route_policy: left_censored_baseline_unless_rib_eor_or_explicit_assumption\n";
        out << "warmup_semantics: score_exclusion_only_not_rib_completeness\n";
        out << "origin_normalization_quality: complete_only_with_complete_relevant_peer_baselines\n";
        out << "pre_post_policy_separation: unsupported_without_bmp_peer_flags\n";
        out << "afi_safi_vrf_scope: prefix_string_only_no_safi_rd_or_peer_distinguisher\n";
        out << "add_path_exactness: unsupported_without_path_identifier\n";
        out << "attribute_scope: as_path,next_hop,communities,origin,med,local_pref,atomic_aggregate,aggregator\n";
        out << "path_as_semantics: membership_in_changed_as_path_not_root_cause\n";
        out << "origin_as_semantics: locally_observed_route_endpoint_not_fault_owner\n";

        print_prefix_tables(out);
        print_peer_as_tables(out);
        print_origin_tables(out);
        print_path_as_tables(out);
    }

   private:
    bool select_router_scope(const BGPMessage &message) {
        const std::string scope = make_router_scope(message);
        observed_router_scopes_.insert(scope);
        if (!selected_router_scope_.has_value()) {
            selected_router_scope_ = scope;
            return true;
        }
        if (!config_.strict_single_router || *selected_router_scope_ == scope) {
            return true;
        }
        ++foreign_router_messages_skipped_;
        return false;
    }

    void observe_time(const EventTime &time) {
        if (!has_observation_time_) {
            has_observation_time_ = true;
            observation_start_ = time;
            observation_end_ = time;
            analysis_start_ = add_seconds(time, config_.warmup_seconds);
            last_arrival_time_ = time;
            has_last_arrival_time_ = true;
            return;
        }
        if (time < last_arrival_time_) {
            ++global_time_order_violations_;
        } else {
            last_arrival_time_ = time;
        }
        if (observation_end_ < time) {
            observation_end_ = time;
        }
        if (time < observation_start_) {
            // Do not move analysis_start_ after processing has begun. This is
            // deliberately diagnostic: the framework is expected to provide
            // chronological input.
            observation_start_ = time;
        }
        update_warmup_completion(time);
    }

    void update_warmup_completion(const EventTime &time) {
        if (warmup_completed_ || !has_observation_time_ || time < analysis_start_) {
            return;
        }
        // Warm-up only controls which events are scored. It does not prove
        // that the Adj-RIB-In is complete: a stable prefix may emit no UPDATE
        // during an arbitrarily long warm-up interval. Completeness is granted
        // only by an explicit RIB/EOR boundary or by the operator's
        // BGP_CHURN_ASSUME_COMPLETE_AT_START assumption.
        warmup_completed_ = true;
    }

    bool is_scored(const EventTime &time) const {
        return has_observation_time_ && !(time < analysis_start_);
    }

    std::size_t get_peer_id(const BGPMessage &message) {
        const PeerIdentity identity = make_peer_identity(message, *selected_router_scope_);
        const auto found = peer_lookup_.find(identity);
        if (found != peer_lookup_.end()) {
            return found->second;
        }

        const std::size_t peer_id = peers_.size();
        PeerData peer;
        peer.identity = identity;
        peer.baseline_complete = config_.assume_complete_at_start && config_.warmup_seconds == 0;
        peers_.push_back(std::move(peer));
        peer_lookup_.emplace(identity, peer_id);
        return peer_id;
    }

    std::size_t get_route_id(std::size_t peer_id, const std::string &prefix) {
        RouteIdentity identity{peer_id, prefix};
        const auto found = route_lookup_.find(identity);
        if (found != route_lookup_.end()) {
            return found->second;
        }

        const std::size_t route_id = route_states_.size();
        RouteState route;
        route.peer_id = peer_id;
        route.prefix = prefix;
        route_states_.push_back(std::move(route));
        route_lookup_.emplace(std::move(identity), route_id);
        return route_id;
    }

    void process_message(const BGPMessage &message) {
        ++messages_seen_;
        if (!select_router_scope(message)) {
            return;
        }

        const EventTime time = message_time(message);
        const OrderKey order = message_order_key(message);
        observe_time(time);
        update_warmup_completion(time);

        const std::size_t peer_id = get_peer_id(message);
        switch (message.type) {
            case BGPMessageType::RIB:
                ++rib_elements_seen_;
                handle_rib(message, peer_id, order);
                break;
            case BGPMessageType::Announcement:
                count_raw_update(message, peer_id, true);
                handle_announcement(message, peer_id, order);
                break;
            case BGPMessageType::Withdrawal:
                count_raw_update(message, peer_id, false);
                handle_withdrawal(message, peer_id, order);
                break;
            case BGPMessageType::PeerState:
                ++peer_state_elements_seen_;
                handle_peer_state(message, peer_id, order);
                break;
            case BGPMessageType::EndOfRib:
                ++end_of_rib_elements_seen_;
                handle_end_of_rib(peer_id, order);
                break;
            case BGPMessageType::Unknown:
                ++unknown_message_types_;
                break;
        }
    }

    void count_raw_update(const BGPMessage &message, std::size_t peer_id, bool announcement) {
        const EventTime time = message_time(message);
        const bool scored = is_scored(time);
        PeerData &peer = peers_[peer_id];

        ++raw_update_nlri_total_;
        ++peer.stats.raw_nlri_total;
        if (scored) {
            ++raw_update_nlri_scored_;
            ++peer.stats.raw_nlri_scored;
            peer.stats.recent_raw.add(time, 1.0, lambda_);
        }

        if (!message.prefix.empty()) {
            PrefixStats &prefix = prefix_stats_[message.prefix];
            ++prefix.raw_total;
            if (announcement) {
                ++prefix.announcements;
            } else {
                ++prefix.withdrawals;
            }
            if (scored) {
                ++prefix.raw_scored;
                prefix.recent_raw.add(time, 1.0, lambda_);
            }
        }

        const std::string source_key = message.source_file.empty()
                                           ? message.project_name + "|" + message.collector_name + "|<stream>"
                                           : message.source_file;
        RecordCursor &cursor = record_cursors_[source_key];
        const bool is_new_record = !cursor.initialized || cursor.current_record_index != message.record_index;
        if (is_new_record) {
            ++update_records_total_;
            ++peer.stats.update_records_total;
            if (scored) {
                ++update_records_scored_;
                ++peer.stats.update_records_scored;
            }
        }

        if (!cursor.initialized) {
            cursor.initialized = true;
            cursor.current_record_index = message.record_index;
            cursor.maximum_record_index = message.record_index;
        } else {
            if (message.record_index < cursor.maximum_record_index) {
                ++record_order_violations_;
            }
            cursor.current_record_index = message.record_index;
            cursor.maximum_record_index = std::max(cursor.maximum_record_index, message.record_index);
        }
    }

    bool route_event_is_late(RouteState &route, PeerData &peer, PrefixStats &prefix, const OrderKey &order) {
        if (route.has_last_order && order < route.last_order) {
            ++route_state_order_violations_;
            ++prefix.out_of_order_events;
            ++peer.stats.out_of_order_events;
            return true;
        }
        if (peer.has_last_any_order && order < peer.last_any_order) {
            // A route-specific event can still be valid when another prefix
            // on the same peer has a later timestamp. Record the condition,
            // but rely on the per-route check for state safety.
            ++peer_cross_prefix_order_observations_;
        }
        route.last_order = order;
        route.has_last_order = true;
        if (!peer.has_last_any_order || peer.last_any_order < order) {
            peer.last_any_order = order;
            peer.has_last_any_order = true;
        }
        return false;
    }

    bool peer_state_event_is_late(PeerData &peer, const OrderKey &order) {
        if (peer.has_last_any_order && order < peer.last_any_order) {
            ++peer_state_order_violations_;
            ++peer.stats.out_of_order_events;
            return true;
        }
        peer.last_any_order = order;
        peer.has_last_any_order = true;
        return false;
    }

    bool peer_sync_active(PeerData &peer, const EventTime &time) {
        if (!peer.sync_active) {
            return false;
        }
        if (config_.session_sync_grace_seconds > 0 &&
            elapsed_seconds(peer.sync_started, time) >
                static_cast<double>(config_.session_sync_grace_seconds)) {
            peer.sync_active = false;
            peer.baseline_complete = true;
            return false;
        }
        return true;
    }

    void handle_rib(const BGPMessage &message, std::size_t peer_id, const OrderKey &order) {
        if (message.prefix.empty()) {
            ++malformed_prefixless_route_events_;
            return;
        }

        PeerData &peer = peers_[peer_id];
        PrefixStats &prefix = prefix_stats_[message.prefix];
        const std::size_t route_id = get_route_id(peer_id, message.prefix);
        RouteState &state = route_states_[route_id];
        if (route_event_is_late(state, peer, prefix, order)) {
            return;
        }

        const EventTime time = order.time;
        const RouteDigest digest = make_route_digest(message);
        if (state.active) {
            close_exposure(state, time);
            peer.active_route_ids.erase(route_id);
            if (is_scored(time)) {
                ++prefix.late_rib_resyncs;
            }
        }

        ++prefix.rib_baselines;
        install_route(state, peer, route_id, digest, time);
        peer.saw_route_event = true;

        if (message.dump_position == BGPDumpPosition::Start ||
            !current_rib_dump_timestamp_.has_value() ||
            *current_rib_dump_timestamp_ != message.dump_timestamp) {
            current_rib_dump_timestamp_ = message.dump_timestamp;
            current_rib_dump_peers_.clear();
        }
        current_rib_dump_peers_.insert(peer_id);
        if (message.dump_position == BGPDumpPosition::End) {
            for (const std::size_t id : current_rib_dump_peers_) {
                peers_[id].baseline_complete = true;
                peers_[id].sync_active = false;
            }
            current_rib_dump_peers_.clear();
        }
    }

    void handle_announcement(const BGPMessage &message, std::size_t peer_id, const OrderKey &order) {
        if (message.prefix.empty()) {
            ++malformed_prefixless_route_events_;
            return;
        }

        PeerData &peer = peers_[peer_id];
        PrefixStats &prefix = prefix_stats_[message.prefix];
        const std::size_t route_id = get_route_id(peer_id, message.prefix);
        RouteState &state = route_states_[route_id];
        if (route_event_is_late(state, peer, prefix, order)) {
            return;
        }

        peer.saw_route_event = true;
        const EventTime time = order.time;
        const bool scored = is_scored(time);
        const bool sync = peer_sync_active(peer, time);
        const RouteDigest new_route = make_route_digest(message);
        credit_raw_announcement(new_route.origin_set, time, scored);

        if (!state.initialized) {
            state.initialized = true;
            if (sync) {
                mark_sync_announcement(prefix, peer, new_route.origin_set, time);
            } else if (scored && peer.baseline_complete) {
                mark_ordinary_new_route(prefix, peer, new_route.origin_set, time);
            } else {
                ++prefix.cold_start_announcements;
                ++cold_start_announcements_;
            }
            install_route(state, peer, route_id, new_route, time);
            return;
        }

        if (!state.active) {
            if (sync) {
                mark_sync_announcement(prefix, peer, new_route.origin_set, time);
            } else if (scored) {
                mark_ordinary_reannouncement(prefix, peer, new_route.origin_set, time);
            }
            install_route(state, peer, route_id, new_route, time);
            return;
        }

        const std::uint32_t mask = route_change_mask(state.route, new_route);
        if (mask == ChangeNone) {
            ++prefix.duplicate_announcements;
            ++peer.stats.duplicate_announcements;
            ++duplicate_announcements_;
            return;
        }

        const RouteDigest old_route = state.route;
        close_exposure(state, time);
        if (sync) {
            mark_sync_replacement(prefix, peer, old_route, new_route, mask, time);
        } else if (scored) {
            mark_ordinary_replacement(prefix, peer, old_route, new_route, mask, time);
        }
        install_route(state, peer, route_id, new_route, time);
    }

    void handle_withdrawal(const BGPMessage &message, std::size_t peer_id, const OrderKey &order) {
        if (message.prefix.empty()) {
            ++malformed_prefixless_route_events_;
            return;
        }

        PeerData &peer = peers_[peer_id];
        PrefixStats &prefix = prefix_stats_[message.prefix];
        const std::size_t route_id = get_route_id(peer_id, message.prefix);
        RouteState &state = route_states_[route_id];
        if (route_event_is_late(state, peer, prefix, order)) {
            return;
        }

        peer.saw_route_event = true;
        const EventTime time = order.time;
        const bool scored = is_scored(time);
        const bool sync = peer_sync_active(peer, time);

        if (!state.initialized) {
            state.initialized = true;
            state.active = false;
            ++prefix.unknown_withdrawals;
            ++peer.stats.unknown_withdrawals;
            ++unknown_withdrawals_;
            return;
        }

        if (!state.active) {
            ++prefix.duplicate_withdrawals;
            ++peer.stats.duplicate_withdrawals;
            ++duplicate_withdrawals_;
            return;
        }

        const std::vector<std::uint32_t> old_origins = state.route.origin_set;
        close_exposure(state, time);
        peer.active_route_ids.erase(route_id);
        state.active = false;

        if (sync) {
            ++prefix.session_sync_withdrawals;
            ++peer.stats.session_sync_withdrawals;
            ++reset_induced_changes_;
            prefix.recent_reset.add(time, 1.0, lambda_);
            credit_origin_reset(old_origins, time, 1.0);
        } else if (scored) {
            ++prefix.ordinary_effective_changes;
            ++prefix.effective_withdrawals;
            ++peer.stats.ordinary_effective_changes;
            ++ordinary_effective_changes_;
            prefix.recent_effective.add(time, 1.0, lambda_);
            peer.stats.recent_effective.add(time, 1.0, lambda_);
            credit_origin_withdrawal(old_origins, time);
        }
    }

    void handle_peer_state(const BGPMessage &message, std::size_t peer_id, const OrderKey &order) {
        PeerData &peer = peers_[peer_id];
        if (peer_state_event_is_late(peer, order)) {
            return;
        }

        peer.saw_peer_state = true;
        const bool old_established = is_established(message.old_peer_state);
        const bool new_established = is_established(message.new_peer_state);
        const bool new_is_known_non_established =
            message.new_peer_state.has_value() && *message.new_peer_state != BGPPeerState::Unknown && !new_established;

        if ((old_established || peer.established) && new_is_known_non_established) {
            ++peer.stats.peer_down_events;
            reset_peer_routes(peer_id, order);
            peer.established = false;
            peer.sync_active = false;
            peer.baseline_complete = false;
        }

        if (new_established) {
            if (peer.established && !peer.active_route_ids.empty()) {
                // A second establishment without a visible down transition is
                // treated as an implicit resynchronization boundary.
                reset_peer_routes(peer_id, order);
            }
            ++peer.stats.peer_up_events;
            peer.established = true;
            peer.sync_active = true;
            peer.baseline_complete = false;
            peer.sync_started = order.time;
        }
    }

    void handle_end_of_rib(std::size_t peer_id, const OrderKey &order) {
        PeerData &peer = peers_[peer_id];
        if (peer_state_event_is_late(peer, order)) {
            return;
        }
        ++peer.stats.end_of_rib_events;
        peer.sync_active = false;
        peer.baseline_complete = true;
    }

    void reset_peer_routes(std::size_t peer_id, const OrderKey &order) {
        PeerData &peer = peers_[peer_id];
        const EventTime time = order.time;
        const bool scored = is_scored(time);
        const std::size_t affected = peer.active_route_ids.size();
        ++peer.stats.session_resets;
        if (affected == 0) {
            return;
        }

        peer.stats.session_reset_routes += static_cast<std::uint64_t>(affected);

        for (const std::size_t route_id : peer.active_route_ids) {
            RouteState &state = route_states_[route_id];
            if (!state.active) {
                continue;
            }
            close_exposure(state, time);
            state.active = false;
            state.last_order = order;
            state.has_last_order = true;

            if (scored) {
                PrefixStats &prefix = prefix_stats_[state.prefix];
                ++prefix.session_reset_withdrawals;
                prefix.recent_reset.add(time, 1.0, lambda_);
                ++reset_induced_changes_;
                credit_origin_reset(state.route.origin_set, time, 1.0);
            }
        }
        peer.active_route_ids.clear();
    }

    void install_route(RouteState &state, PeerData &peer, std::size_t route_id, const RouteDigest &route,
                       const EventTime &time) {
        state.route = route;
        state.initialized = true;
        state.active = true;
        state.active_since = time < analysis_start_ ? analysis_start_ : time;
        peer.active_route_ids.insert(route_id);
    }

    void close_exposure(const RouteState &state, const EventTime &time) {
        if (!state.active || state.route.origin_set.empty()) {
            return;
        }
        const EventTime start = state.active_since < analysis_start_ ? analysis_start_ : state.active_since;
        if (!(start < time)) {
            return;
        }
        add_origin_route_seconds(state.route.origin_set, elapsed_seconds(start, time));
    }

    void add_origin_route_seconds(const std::vector<std::uint32_t> &origins, double seconds) {
        if (origins.empty() || !(seconds > 0.0)) {
            return;
        }
        const double share = seconds / static_cast<double>(origins.size());
        for (const std::uint32_t asn : origins) {
            origin_stats_[asn].route_seconds_closed += share;
        }
    }

    template <typename Function>
    void for_each_origin_share(const std::vector<std::uint32_t> &origins, double total_mass, Function &&function) {
        if (origins.empty()) {
            ++unknown_origin_events_;
            return;
        }
        const double share = total_mass / static_cast<double>(origins.size());
        for (const std::uint32_t asn : origins) {
            function(origin_stats_[asn], share);
        }
    }

    void credit_raw_announcement(const std::vector<std::uint32_t> &origins, const EventTime &time, bool scored) {
        (void)time;
        for_each_origin_share(origins, 1.0, [&](OriginStats &stats, double share) {
            stats.raw_announcement_mass_total += share;
            if (scored) {
                stats.raw_announcement_mass_scored += share;
            }
        });
    }

    void credit_origin_effective(const std::vector<std::uint32_t> &origins, const EventTime &time, double total_mass,
                                 void (*category)(OriginStats &, double)) {
        for_each_origin_share(origins, total_mass, [&](OriginStats &stats, double share) {
            stats.ordinary_effective_mass += share;
            stats.recent_effective.add(time, share, lambda_);
            category(stats, share);
        });
    }

    static void category_new(OriginStats &stats, double share) { stats.new_route_mass += share; }
    static void category_reannouncement(OriginStats &stats, double share) { stats.reannouncement_mass += share; }
    static void category_withdrawal(OriginStats &stats, double share) { stats.withdrawal_mass += share; }
    static void category_replacement(OriginStats &stats, double share) { stats.replacement_mass += share; }
    static void category_origin_in(OriginStats &stats, double share) { stats.origin_change_in_mass += share; }
    static void category_origin_out(OriginStats &stats, double share) { stats.origin_change_out_mass += share; }

    void credit_origin_reset(const std::vector<std::uint32_t> &origins, const EventTime &time, double total_mass) {
        for_each_origin_share(origins, total_mass, [&](OriginStats &stats, double share) {
            stats.reset_induced_mass += share;
            stats.recent_reset.add(time, share, lambda_);
        });
    }

    void credit_origin_withdrawal(const std::vector<std::uint32_t> &origins, const EventTime &time) {
        credit_origin_effective(origins, time, 1.0, &category_withdrawal);
    }

    void credit_origin_replacement(const RouteDigest &old_route, const RouteDigest &new_route, std::uint32_t mask,
                                   const EventTime &time) {
        auto contains_asn = [](const std::vector<std::uint32_t> &origins, std::uint32_t asn) {
            return std::binary_search(origins.begin(), origins.end(), asn);
        };
        auto credit_side = [&](const std::vector<std::uint32_t> &origins,
                               const std::vector<std::uint32_t> &other_origins, double total_mass,
                               bool is_new_side) {
            if (origins.empty()) {
                ++unknown_origin_events_;
                return;
            }
            const double share = total_mass / static_cast<double>(origins.size());
            for (const std::uint32_t asn : origins) {
                OriginStats &stats = origin_stats_[asn];
                stats.ordinary_effective_mass += share;
                stats.recent_effective.add(time, share, lambda_);
                stats.replacement_mass += share;
                if ((mask & ChangeASPath) != 0U) {
                    stats.path_change_mass += share;
                }
                if (has_non_path_change(mask)) {
                    stats.attribute_change_mass += share;
                }
                // A member retained in both MOAS endpoint sets neither enters
                // nor leaves the origin set. Only the set difference receives
                // origin-in/origin-out attribution.
                if (!contains_asn(other_origins, asn)) {
                    if (is_new_side) {
                        stats.origin_change_in_mass += share;
                    } else {
                        stats.origin_change_out_mass += share;
                    }
                }
            }
        };

        if (old_route.origin_set == new_route.origin_set) {
            credit_side(new_route.origin_set, old_route.origin_set, 1.0, true);
            return;
        }

        if (!old_route.origin_set.empty() && !new_route.origin_set.empty()) {
            credit_side(old_route.origin_set, new_route.origin_set, 0.5, false);
            credit_side(new_route.origin_set, old_route.origin_set, 0.5, true);
        } else if (!old_route.origin_set.empty()) {
            credit_side(old_route.origin_set, new_route.origin_set, 1.0, false);
        } else {
            credit_side(new_route.origin_set, old_route.origin_set, 1.0, true);
        }
    }

    void mark_ordinary_new_route(PrefixStats &prefix, PeerData &peer, const std::vector<std::uint32_t> &origins,
                                 const EventTime &time) {
        ++prefix.ordinary_effective_changes;
        ++prefix.new_routes;
        ++peer.stats.ordinary_effective_changes;
        ++ordinary_effective_changes_;
        prefix.recent_effective.add(time, 1.0, lambda_);
        peer.stats.recent_effective.add(time, 1.0, lambda_);
        credit_origin_effective(origins, time, 1.0, &category_new);
    }

    void mark_ordinary_reannouncement(PrefixStats &prefix, PeerData &peer,
                                      const std::vector<std::uint32_t> &origins, const EventTime &time) {
        ++prefix.ordinary_effective_changes;
        ++prefix.reannouncements;
        ++peer.stats.ordinary_effective_changes;
        ++ordinary_effective_changes_;
        prefix.recent_effective.add(time, 1.0, lambda_);
        peer.stats.recent_effective.add(time, 1.0, lambda_);
        credit_origin_effective(origins, time, 1.0, &category_reannouncement);
    }

    void credit_path_as_change(const RouteDigest &old_route, const RouteDigest &new_route,
                               const EventTime &time, bool session_induced) {
        if (old_route.path_member_set.empty() && new_route.path_member_set.empty()) {
            return;
        }

        std::vector<std::uint32_t> all_members;
        std::set_union(old_route.path_member_set.begin(), old_route.path_member_set.end(),
                       new_route.path_member_set.begin(), new_route.path_member_set.end(),
                       std::back_inserter(all_members));
        for (const std::uint32_t asn : all_members) {
            PathAsStats &stats = path_as_stats_[asn];
            if (session_induced) {
                ++stats.session_induced_involvement;
                continue;
            }
            ++stats.changed_path_involvement;
            stats.recent_changed_path.add(time, 1.0, lambda_);
            const bool in_old = std::binary_search(old_route.path_member_set.begin(),
                                                   old_route.path_member_set.end(), asn);
            const bool in_new = std::binary_search(new_route.path_member_set.begin(),
                                                   new_route.path_member_set.end(), asn);
            if (!in_old && in_new) {
                ++stats.added_to_path;
            } else if (in_old && !in_new) {
                ++stats.removed_from_path;
            } else {
                // Includes ASes retained while ordering/prepending elsewhere
                // changed. This is association, not causal attribution.
                ++stats.retained_in_changed_path;
            }
        }
    }

    void mark_ordinary_replacement(PrefixStats &prefix, PeerData &peer, const RouteDigest &old_route,
                                   const RouteDigest &new_route, std::uint32_t mask, const EventTime &time) {
        ++prefix.ordinary_effective_changes;
        ++prefix.replacements;
        ++peer.stats.ordinary_effective_changes;
        ++ordinary_effective_changes_;
        prefix.recent_effective.add(time, 1.0, lambda_);
        peer.stats.recent_effective.add(time, 1.0, lambda_);
        update_prefix_change_categories(prefix, mask);
        if ((mask & ChangeASPath) != 0U) {
            credit_path_as_change(old_route, new_route, time, false);
        }
        credit_origin_replacement(old_route, new_route, mask, time);
    }

    void mark_sync_announcement(PrefixStats &prefix, PeerData &peer, const std::vector<std::uint32_t> &origins,
                                const EventTime &time) {
        ++prefix.session_sync_announcements;
        ++peer.stats.session_sync_announcements;
        ++reset_induced_changes_;
        prefix.recent_reset.add(time, 1.0, lambda_);
        credit_origin_reset(origins, time, 1.0);
    }

    void mark_sync_replacement(PrefixStats &prefix, PeerData &peer, const RouteDigest &old_route,
                               const RouteDigest &new_route, std::uint32_t mask, const EventTime &time) {
        ++prefix.session_sync_replacements;
        ++peer.stats.session_sync_replacements;
        ++reset_induced_changes_;
        prefix.recent_reset.add(time, 1.0, lambda_);
        if ((mask & ChangeASPath) != 0U) {
            credit_path_as_change(old_route, new_route, time, true);
        }
        if (old_route.origin_set == new_route.origin_set) {
            credit_origin_reset(new_route.origin_set, time, 1.0);
        } else if (!old_route.origin_set.empty() && !new_route.origin_set.empty()) {
            credit_origin_reset(old_route.origin_set, time, 0.5);
            credit_origin_reset(new_route.origin_set, time, 0.5);
        } else if (!old_route.origin_set.empty()) {
            credit_origin_reset(old_route.origin_set, time, 1.0);
        } else {
            credit_origin_reset(new_route.origin_set, time, 1.0);
        }
    }

    static void update_prefix_change_categories(PrefixStats &prefix, std::uint32_t mask) {
        if ((mask & ChangeASPath) != 0U) {
            ++prefix.path_changes;
        }
        if ((mask & ChangeOriginSet) != 0U) {
            ++prefix.origin_set_changes;
        }
        if ((mask & ChangeNextHop) != 0U) {
            ++prefix.next_hop_changes;
        }
        if ((mask & ChangeCommunities) != 0U) {
            ++prefix.community_changes;
        }
        if ((mask & ChangeOriginAttribute) != 0U) {
            ++prefix.origin_attribute_changes;
        }
        if ((mask & ChangeMED) != 0U) {
            ++prefix.med_changes;
        }
        if ((mask & ChangeLocalPref) != 0U) {
            ++prefix.local_pref_changes;
        }
        if ((mask & ChangeAtomicAggregate) != 0U) {
            ++prefix.atomic_aggregate_changes;
        }
        if ((mask & ChangeAggregator) != 0U) {
            ++prefix.aggregator_changes;
        }
    }

    std::size_t baseline_complete_peer_count() const {
        return static_cast<std::size_t>(std::count_if(peers_.begin(), peers_.end(), [](const PeerData &peer) {
            return peer.baseline_complete;
        }));
    }

    std::size_t synchronizing_peer_count() const {
        return static_cast<std::size_t>(std::count_if(peers_.begin(), peers_.end(), [](const PeerData &peer) {
            return peer.sync_active;
        }));
    }

    std::size_t active_route_count() const {
        std::size_t total = 0;
        for (const PeerData &peer : peers_) {
            total += peer.active_route_ids.size();
        }
        return total;
    }

    double analysis_hours() const {
        if (!has_observation_time_ || !(analysis_start_ < observation_end_)) {
            return 0.0;
        }
        return elapsed_seconds(analysis_start_, observation_end_) / 3600.0;
    }

    std::unordered_map<std::uint32_t, double> current_origin_route_seconds() const {
        std::unordered_map<std::uint32_t, double> route_seconds;
        route_seconds.reserve(origin_stats_.size() * 2U + 1U);
        for (const auto &[asn, stats] : origin_stats_) {
            route_seconds[asn] += stats.route_seconds_closed;
        }

        if (!has_observation_time_) {
            return route_seconds;
        }
        for (const RouteState &state : route_states_) {
            if (!state.active || state.route.origin_set.empty()) {
                continue;
            }
            const EventTime start = state.active_since < analysis_start_ ? analysis_start_ : state.active_since;
            if (!(start < observation_end_)) {
                continue;
            }
            const double seconds = elapsed_seconds(start, observation_end_);
            const double share = seconds / static_cast<double>(state.route.origin_set.size());
            for (const std::uint32_t asn : state.route.origin_set) {
                route_seconds[asn] += share;
            }
        }
        return route_seconds;
    }

    template <typename Item, typename Score>
    static void retain_top_k(std::vector<Item> &items, std::size_t top_k, Score score) {
        const auto comparator = [&](const Item &lhs, const Item &rhs) {
            const double lhs_score = score(lhs);
            const double rhs_score = score(rhs);
            if (lhs_score != rhs_score) {
                return lhs_score > rhs_score;
            }
            return lhs.first < rhs.first;
        };
        if (items.size() > top_k) {
            std::partial_sort(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(top_k), items.end(), comparator);
            items.resize(top_k);
        } else {
            std::sort(items.begin(), items.end(), comparator);
        }
    }

    void print_prefix_tables(std::ostream &out) const {
        using PrefixItem = std::pair<std::string, const PrefixStats *>;
        std::vector<PrefixItem> items;
        items.reserve(prefix_stats_.size());
        for (const auto &[prefix, stats] : prefix_stats_) {
            items.emplace_back(prefix, &stats);
        }

        auto print_header = [&](std::string_view title) {
            out << "\n[" << title << "]\n";
            out << "rank\tprefix\traw_total\traw_scored\tannouncements\twithdrawals\teffective"
                   "\tavg_effective_per_hour\trecent_effective_per_hour\trib_baseline\tcold_start"
                   "\tnew\treannounce\twithdraw\treplace\tdup_a\tdup_w\tunknown_w\tpath\torigin_set"
                   "\tnext_hop\tcommunities\torigin_attr\tmed\tlocal_pref\tatomic\taggregator"
                   "\treset_withdraw\tsync_announce\tsync_replace\tsync_withdraw\tlate_rib_resync"
                   "\tout_of_order\n";
        };
        auto print_rows = [&](const std::vector<PrefixItem> &rows) {
            const double hours = analysis_hours();
            std::size_t rank = 1;
            for (const PrefixItem &item : rows) {
                const PrefixStats &s = *item.second;
                out << rank++ << '\t' << item.first << '\t' << s.raw_total << '\t' << s.raw_scored << '\t'
                    << s.announcements << '\t' << s.withdrawals << '\t' << s.ordinary_effective_changes << '\t'
                    << (hours > 0.0 ? static_cast<double>(s.ordinary_effective_changes) / hours : 0.0) << '\t'
                    << s.recent_effective.rate_per_hour(observation_end_, lambda_) << '\t' << s.rib_baselines << '\t'
                    << s.cold_start_announcements << '\t' << s.new_routes << '\t' << s.reannouncements << '\t'
                    << s.effective_withdrawals << '\t' << s.replacements << '\t'
                    << s.duplicate_announcements << '\t' << s.duplicate_withdrawals << '\t'
                    << s.unknown_withdrawals << '\t' << s.path_changes << '\t' << s.origin_set_changes << '\t'
                    << s.next_hop_changes << '\t' << s.community_changes << '\t'
                    << s.origin_attribute_changes << '\t' << s.med_changes << '\t'
                    << s.local_pref_changes << '\t' << s.atomic_aggregate_changes << '\t'
                    << s.aggregator_changes << '\t' << s.session_reset_withdrawals << '\t'
                    << s.session_sync_announcements << '\t' << s.session_sync_replacements << '\t'
                    << s.session_sync_withdrawals << '\t' << s.late_rib_resyncs << '\t'
                    << s.out_of_order_events << '\n';
            }
        };

        std::vector<PrefixItem> by_raw = items;
        retain_top_k(by_raw, config_.top_k,
                     [](const PrefixItem &item) { return static_cast<double>(item.second->raw_scored); });
        print_header("top_prefixes_by_raw_nlri");
        print_rows(by_raw);

        std::vector<PrefixItem> by_effective = items;
        retain_top_k(by_effective, config_.top_k,
                     [](const PrefixItem &item) { return static_cast<double>(item.second->ordinary_effective_changes); });
        print_header("top_prefixes_by_ordinary_effective_changes");
        print_rows(by_effective);

        std::vector<PrefixItem> by_recent = items;
        retain_top_k(by_recent, config_.top_k, [&](const PrefixItem &item) {
            return item.second->recent_effective.rate_per_hour(observation_end_, lambda_);
        });
        print_header("top_prefixes_by_recent_effective_rate");
        print_rows(by_recent);

        std::vector<PrefixItem> by_reset = items;
        retain_top_k(by_reset, config_.top_k, [](const PrefixItem &item) {
            return static_cast<double>(item.second->session_reset_withdrawals +
                                       item.second->session_sync_announcements +
                                       item.second->session_sync_replacements +
                                       item.second->session_sync_withdrawals);
        });
        print_header("top_prefixes_by_session_induced_changes");
        print_rows(by_reset);
    }

    std::unordered_map<std::uint32_t, PeerAsAggregate> aggregate_peer_asns() const {
        std::unordered_map<std::uint32_t, PeerAsAggregate> result;
        for (const PeerData &peer : peers_) {
            PeerAsAggregate &aggregate = result[peer.identity.peer_asn];
            ++aggregate.sessions;
            aggregate.update_records_total += peer.stats.update_records_total;
            aggregate.update_records_scored += peer.stats.update_records_scored;
            aggregate.raw_nlri_total += peer.stats.raw_nlri_total;
            aggregate.raw_nlri_scored += peer.stats.raw_nlri_scored;
            aggregate.ordinary_effective_changes += peer.stats.ordinary_effective_changes;
            aggregate.session_resets += peer.stats.session_resets;
            aggregate.session_reset_routes += peer.stats.session_reset_routes;
            aggregate.session_sync_announcements += peer.stats.session_sync_announcements;
            aggregate.duplicates += peer.stats.duplicate_announcements + peer.stats.duplicate_withdrawals;
            aggregate.recent_raw_rate += peer.stats.recent_raw.rate_per_hour(observation_end_, lambda_);
            aggregate.recent_effective_rate += peer.stats.recent_effective.rate_per_hour(observation_end_, lambda_);
        }
        return result;
    }

    void print_peer_as_tables(std::ostream &out) const {
        using PeerItem = std::pair<std::uint32_t, PeerAsAggregate>;
        const auto aggregate = aggregate_peer_asns();
        std::vector<PeerItem> items;
        items.reserve(aggregate.size());
        for (const auto &[asn, stats] : aggregate) {
            items.emplace_back(asn, stats);
        }

        auto print_header = [&](std::string_view title) {
            out << "\n[" << title << "]\n";
            out << "rank\tpeer_asn\tsessions\tupdate_records_scored\traw_nlri_scored\teffective"
                   "\trecent_raw_per_hour\trecent_effective_per_hour\tresets\treset_routes\tsync_announcements"
                   "\tduplicates\n";
        };
        auto print_rows = [&](const std::vector<PeerItem> &rows) {
            std::size_t rank = 1;
            for (const PeerItem &item : rows) {
                const PeerAsAggregate &s = item.second;
                out << rank++ << '\t' << item.first << '\t' << s.sessions << '\t' << s.update_records_scored << '\t'
                    << s.raw_nlri_scored << '\t' << s.ordinary_effective_changes << '\t' << s.recent_raw_rate << '\t'
                    << s.recent_effective_rate << '\t' << s.session_resets << '\t' << s.session_reset_routes << '\t'
                    << s.session_sync_announcements << '\t' << s.duplicates << '\n';
            }
        };

        std::vector<PeerItem> by_records = items;
        retain_top_k(by_records, config_.top_k,
                     [](const PeerItem &item) { return static_cast<double>(item.second.update_records_scored); });
        print_header("top_peer_asns_by_update_records");
        print_rows(by_records);

        std::vector<PeerItem> by_nlri = items;
        retain_top_k(by_nlri, config_.top_k,
                     [](const PeerItem &item) { return static_cast<double>(item.second.raw_nlri_scored); });
        print_header("top_peer_asns_by_nlri_elements");
        print_rows(by_nlri);

        std::vector<PeerItem> by_effective = items;
        retain_top_k(by_effective, config_.top_k,
                     [](const PeerItem &item) { return static_cast<double>(item.second.ordinary_effective_changes); });
        print_header("top_peer_asns_by_ordinary_effective_changes");
        print_rows(by_effective);

        std::vector<PeerItem> by_reset_routes = items;
        retain_top_k(by_reset_routes, config_.top_k,
                     [](const PeerItem &item) { return static_cast<double>(item.second.session_reset_routes); });
        print_header("top_peer_asns_by_session_reset_routes");
        print_rows(by_reset_routes);
    }

    void print_origin_tables(std::ostream &out) const {
        struct OriginRow {
            std::uint32_t asn = 0;
            const OriginStats *stats = nullptr;
            double route_hours = 0.0;
            double normalized_per_1000_route_hours = 0.0;
            double recent_effective_rate = 0.0;
        };

        const auto route_seconds = current_origin_route_seconds();
        std::vector<std::pair<std::uint32_t, OriginRow>> items;
        items.reserve(origin_stats_.size());
        for (const auto &[asn, stats] : origin_stats_) {
            const auto found = route_seconds.find(asn);
            const double route_hours = found == route_seconds.end() ? 0.0 : found->second / 3600.0;
            OriginRow row;
            row.asn = asn;
            row.stats = &stats;
            row.route_hours = route_hours;
            row.normalized_per_1000_route_hours =
                route_hours > 0.0 ? 1000.0 * stats.ordinary_effective_mass / route_hours : 0.0;
            row.recent_effective_rate = stats.recent_effective.rate_per_hour(observation_end_, lambda_);
            items.emplace_back(asn, row);
        }

        auto print_header = [&](std::string_view title) {
            out << "\n[" << title << "]\n";
            out << "rank\torigin_asn\traw_announcement_mass\teffective_mass\trecent_effective_per_hour"
                   "\tsession_induced_mass\troute_hours\tevents_per_1000_route_hours\tnew\treannounce\twithdraw"
                   "\treplace\tpath_change\tattribute_change\torigin_in\torigin_out\n";
        };
        auto print_rows = [&](const std::vector<std::pair<std::uint32_t, OriginRow>> &rows) {
            std::size_t rank = 1;
            for (const auto &item : rows) {
                const OriginRow &row = item.second;
                const OriginStats &s = *row.stats;
                out << rank++ << '\t' << row.asn << '\t' << s.raw_announcement_mass_scored << '\t'
                    << s.ordinary_effective_mass << '\t' << row.recent_effective_rate << '\t'
                    << s.reset_induced_mass << '\t' << row.route_hours << '\t'
                    << row.normalized_per_1000_route_hours << '\t' << s.new_route_mass << '\t'
                    << s.reannouncement_mass << '\t' << s.withdrawal_mass << '\t' << s.replacement_mass << '\t'
                    << s.path_change_mass << '\t' << s.attribute_change_mass << '\t' << s.origin_change_in_mass
                    << '\t' << s.origin_change_out_mass << '\n';
            }
        };

        auto by_raw = items;
        retain_top_k(by_raw, config_.top_k, [](const auto &item) {
            return item.second.stats->raw_announcement_mass_scored;
        });
        print_header("top_origin_asns_by_raw_announcement_mass");
        print_rows(by_raw);

        auto by_effective = items;
        retain_top_k(by_effective, config_.top_k, [](const auto &item) {
            return item.second.stats->ordinary_effective_mass;
        });
        print_header("top_origin_asns_by_effective_event_mass");
        print_rows(by_effective);

        auto by_recent = items;
        retain_top_k(by_recent, config_.top_k,
                     [](const auto &item) { return item.second.recent_effective_rate; });
        print_header("top_origin_asns_by_recent_effective_rate");
        print_rows(by_recent);

        auto by_session = items;
        retain_top_k(by_session, config_.top_k, [](const auto &item) {
            return item.second.stats->reset_induced_mass;
        });
        print_header("top_origin_asns_by_session_induced_mass");
        print_rows(by_session);

        std::vector<std::pair<std::uint32_t, OriginRow>> by_normalized;
        by_normalized.reserve(items.size());
        for (const auto &item : items) {
            if (item.second.route_hours >= config_.minimum_origin_route_hours) {
                by_normalized.push_back(item);
            }
        }
        retain_top_k(by_normalized, config_.top_k,
                     [](const auto &item) { return item.second.normalized_per_1000_route_hours; });
        print_header("top_origin_asns_by_normalized_effective_rate");
        print_rows(by_normalized);
    }

    void print_path_as_tables(std::ostream &out) const {
        using PathItem = std::pair<std::uint32_t, const PathAsStats *>;
        std::vector<PathItem> items;
        items.reserve(path_as_stats_.size());
        for (const auto &[asn, stats] : path_as_stats_) {
            items.emplace_back(asn, &stats);
        }

        auto print_header = [&](std::string_view title) {
            out << "\n[" << title << "]\n";
            out << "rank\tpath_asn\tchanged_path_involvement\trecent_involvement_per_hour"
                   "\tadded\tremoved\tretained_in_changed_path\tsession_induced_involvement\n";
        };
        auto print_rows = [&](const std::vector<PathItem> &rows) {
            std::size_t rank = 1;
            for (const PathItem &item : rows) {
                const PathAsStats &stats = *item.second;
                out << rank++ << '\t' << item.first << '\t' << stats.changed_path_involvement << '\t'
                    << stats.recent_changed_path.rate_per_hour(observation_end_, lambda_) << '\t'
                    << stats.added_to_path << '\t' << stats.removed_from_path << '\t'
                    << stats.retained_in_changed_path << '\t' << stats.session_induced_involvement << '\n';
            }
        };

        auto by_total = items;
        retain_top_k(by_total, config_.top_k, [](const PathItem &item) {
            return static_cast<double>(item.second->changed_path_involvement);
        });
        print_header("top_path_member_asns_by_changed_path_involvement");
        print_rows(by_total);

        auto by_recent = items;
        retain_top_k(by_recent, config_.top_k, [&](const PathItem &item) {
            return item.second->recent_changed_path.rate_per_hour(observation_end_, lambda_);
        });
        print_header("top_path_member_asns_by_recent_changed_path_involvement");
        print_rows(by_recent);

        auto by_session = items;
        retain_top_k(by_session, config_.top_k, [](const PathItem &item) {
            return static_cast<double>(item.second->session_induced_involvement);
        });
        print_header("top_path_member_asns_by_session_induced_involvement");
        print_rows(by_session);
    }

    RuntimeConfig config_;
    double lambda_ = 0.0;
    bool finalized_ = false;

    std::optional<std::string> selected_router_scope_;
    std::unordered_set<std::string> observed_router_scopes_;
    std::uint64_t foreign_router_messages_skipped_ = 0;

    bool has_observation_time_ = false;
    EventTime observation_start_{};
    EventTime analysis_start_{};
    EventTime observation_end_{};
    EventTime last_arrival_time_{};
    bool has_last_arrival_time_ = false;
    bool warmup_completed_ = false;

    std::unordered_map<PeerIdentity, std::size_t, PeerIdentityHash> peer_lookup_;
    std::vector<PeerData> peers_;
    std::unordered_map<RouteIdentity, std::size_t, RouteIdentityHash> route_lookup_;
    std::vector<RouteState> route_states_;
    std::unordered_map<std::string, PrefixStats> prefix_stats_;
    std::unordered_map<std::uint32_t, OriginStats> origin_stats_;
    std::unordered_map<std::uint32_t, PathAsStats> path_as_stats_;
    std::unordered_map<std::string, RecordCursor> record_cursors_;

    std::optional<std::time_t> current_rib_dump_timestamp_;
    std::unordered_set<std::size_t> current_rib_dump_peers_;

    std::uint64_t messages_seen_ = 0;
    std::uint64_t raw_update_nlri_total_ = 0;
    std::uint64_t raw_update_nlri_scored_ = 0;
    std::uint64_t update_records_total_ = 0;
    std::uint64_t update_records_scored_ = 0;
    std::uint64_t rib_elements_seen_ = 0;
    std::uint64_t peer_state_elements_seen_ = 0;
    std::uint64_t end_of_rib_elements_seen_ = 0;
    std::uint64_t unknown_message_types_ = 0;
    std::uint64_t ordinary_effective_changes_ = 0;
    std::uint64_t reset_induced_changes_ = 0;
    std::uint64_t duplicate_announcements_ = 0;
    std::uint64_t duplicate_withdrawals_ = 0;
    std::uint64_t unknown_withdrawals_ = 0;
    std::uint64_t cold_start_announcements_ = 0;
    std::uint64_t unknown_origin_events_ = 0;
    std::uint64_t global_time_order_violations_ = 0;
    std::uint64_t route_state_order_violations_ = 0;
    std::uint64_t peer_state_order_violations_ = 0;
    std::uint64_t peer_cross_prefix_order_observations_ = 0;
    std::uint64_t record_order_violations_ = 0;
    std::uint64_t malformed_prefixless_route_events_ = 0;
};

BGPSTREAM_RUNNER_EXPORT_PROCESSOR(SingleRouterChurnProcessor)
