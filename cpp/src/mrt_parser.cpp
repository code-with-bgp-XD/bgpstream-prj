extern "C" {
#include <bgpstream.h>
}

#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bgpstream_runner/mrt_parser.h"

namespace bgpstream_runner {

namespace {

std::string record_status_to_string(bgpstream_record_status_t status) {
    char buffer[128];
    if (bgpstream_record_status_snprintf(buffer, sizeof(buffer), status) < 0) {
        return "unknown-record-status";
    }
    return std::string(buffer);
}

std::string prefix_to_string(const bgpstream_pfx_t &prefix) {
    char buffer[128];
    if (bgpstream_pfx_snprintf(buffer, sizeof(buffer), &prefix) == nullptr) {
        throw MrtParseFailure(std::string("Failed to stringify prefix: ") + std::strerror(errno));
    }
    return std::string(buffer);
}

std::string address_to_string(const bgpstream_ip_addr_t &address) {
    if (address.version == BGPSTREAM_ADDR_VERSION_UNKNOWN) {
        return {};
    }

    char buffer[INET6_ADDRSTRLEN];
    if (bgpstream_addr_ntop(buffer, sizeof(buffer), &address) == nullptr) {
        throw MrtParseFailure(std::string("Failed to stringify IP address: ") + std::strerror(errno));
    }
    return std::string(buffer);
}

std::string as_path_to_string(const bgpstream_as_path_t *as_path) {
    if (as_path == nullptr) {
        return {};
    }

    std::vector<char> buffer(256);
    while (true) {
        const int required = bgpstream_as_path_snprintf(buffer.data(), buffer.size(), as_path);
        if (required < 0) {
            throw MrtParseFailure("Failed to stringify AS path");
        }
        if (static_cast<std::size_t>(required) < buffer.size()) {
            return std::string(buffer.data(), static_cast<std::size_t>(required));
        }
        buffer.resize(static_cast<std::size_t>(required) + 1);
    }
}

ASPathSegmentType as_path_segment_type_from_bgpstream(std::uint8_t type) {
    switch (type) {
        case BGPSTREAM_AS_PATH_SEG_ASN:
            return ASPathSegmentType::ASN;
        case BGPSTREAM_AS_PATH_SEG_SET:
            return ASPathSegmentType::Set;
        case BGPSTREAM_AS_PATH_SEG_CONFED_SEQ:
            return ASPathSegmentType::ConfederationSequence;
        case BGPSTREAM_AS_PATH_SEG_CONFED_SET:
            return ASPathSegmentType::ConfederationSet;
        default:
            return ASPathSegmentType::Unknown;
    }
}

void append_requested_as_path(bgpstream_as_path_t *as_path, BGPMessageFields fields, BGPMessage *message) {
    if (as_path == nullptr) {
        return;
    }

    if (has_message_field(fields, BGPMessageFields::HasASPath)) {
        message->has_as_path = true;
    }
    if (has_message_field(fields, BGPMessageFields::ASPathString)) {
        message->as_path = as_path_to_string(as_path);
    }

    const bool needs_segments = has_message_field(fields, BGPMessageFields::ASPathSegments);
    const bool needs_flattened_asns = has_message_field(fields, BGPMessageFields::FlattenedAsns);
    if (needs_segments || needs_flattened_asns) {
        const int segment_count = bgpstream_as_path_get_len(as_path);
        if (segment_count > 0) {
            if (needs_segments) {
                message->as_path_segments.reserve(static_cast<std::size_t>(segment_count));
            }
            if (needs_flattened_asns) {
                message->asns.reserve(static_cast<std::size_t>(segment_count));
            }
        }

        bgpstream_as_path_iter_t iter;
        bgpstream_as_path_iter_reset(&iter);
        while (bgpstream_as_path_seg_t *segment = bgpstream_as_path_get_next_seg(as_path, &iter)) {
            if (needs_segments) {
                ASPathSegment output_segment;
                output_segment.type = as_path_segment_type_from_bgpstream(segment->type);
                if (segment->type == BGPSTREAM_AS_PATH_SEG_ASN) {
                    output_segment.asns.push_back(segment->asn.asn);
                    if (needs_flattened_asns) {
                        message->asns.push_back(segment->asn.asn);
                    }
                } else {
                    output_segment.asns.reserve(segment->set.asn_cnt);
                    for (std::uint8_t index = 0; index < segment->set.asn_cnt; ++index) {
                        output_segment.asns.push_back(segment->set.asn[index]);
                        if (needs_flattened_asns) {
                            message->asns.push_back(segment->set.asn[index]);
                        }
                    }
                }
                message->as_path_segments.push_back(std::move(output_segment));
            } else if (segment->type == BGPSTREAM_AS_PATH_SEG_ASN) {
                message->asns.push_back(segment->asn.asn);
            } else {
                for (std::uint8_t index = 0; index < segment->set.asn_cnt; ++index) {
                    message->asns.push_back(segment->set.asn[index]);
                }
            }
        }
    }

    if (has_message_field(fields, BGPMessageFields::OriginAsn)) {
        std::uint32_t origin_asn = 0;
        if (bgpstream_as_path_get_origin_val(as_path, &origin_asn) == 0) {
            message->origin_asn = origin_asn;
        }
    }
}

void append_communities(const bgpstream_community_set_t *community_set, BGPMessage *message) {
    if (community_set == nullptr) {
        return;
    }

    const int community_count = bgpstream_community_set_size(community_set);
    if (community_count <= 0) {
        return;
    }

    message->communities.reserve(static_cast<std::size_t>(community_count));
    for (int index = 0; index < community_count; ++index) {
        const bgpstream_community_t *community = bgpstream_community_set_get(community_set, index);
        if (community == nullptr) {
            throw MrtParseFailure("Failed to read community from community set");
        }
        message->communities.push_back(BGPCommunity{community->asn, community->value});
    }
}

BGPRecordType record_type_from_bgpstream(bgpstream_record_type_t type) {
    switch (type) {
        case BGPSTREAM_UPDATE:
            return BGPRecordType::Update;
        case BGPSTREAM_RIB:
            return BGPRecordType::RIB;
        default:
            return BGPRecordType::Unknown;
    }
}

BGPRecordStatus record_status_from_bgpstream(bgpstream_record_status_t status) {
    switch (status) {
        case BGPSTREAM_RECORD_STATUS_VALID_RECORD:
            return BGPRecordStatus::Valid;
        case BGPSTREAM_RECORD_STATUS_FILTERED_SOURCE:
            return BGPRecordStatus::FilteredSource;
        case BGPSTREAM_RECORD_STATUS_EMPTY_SOURCE:
            return BGPRecordStatus::EmptySource;
        case BGPSTREAM_RECORD_STATUS_OUTSIDE_TIME_INTERVAL:
            return BGPRecordStatus::OutsideTimeInterval;
        case BGPSTREAM_RECORD_STATUS_CORRUPTED_SOURCE:
            return BGPRecordStatus::CorruptedSource;
        case BGPSTREAM_RECORD_STATUS_CORRUPTED_RECORD:
            return BGPRecordStatus::CorruptedRecord;
        case BGPSTREAM_RECORD_STATUS_UNSUPPORTED_RECORD:
            return BGPRecordStatus::UnsupportedRecord;
        default:
            return BGPRecordStatus::Unknown;
    }
}

BGPDumpPosition dump_position_from_bgpstream(bgpstream_dump_position_t position) {
    switch (position) {
        case BGPSTREAM_DUMP_START:
            return BGPDumpPosition::Start;
        case BGPSTREAM_DUMP_MIDDLE:
            return BGPDumpPosition::Middle;
        case BGPSTREAM_DUMP_END:
            return BGPDumpPosition::End;
        default:
            return BGPDumpPosition::Unknown;
    }
}

BGPOrigin origin_from_bgpstream(bgpstream_elem_origin_type_t origin) {
    switch (origin) {
        case BGPSTREAM_ELEM_BGP_UPDATE_ORIGIN_IGP:
            return BGPOrigin::IGP;
        case BGPSTREAM_ELEM_BGP_UPDATE_ORIGIN_EGP:
            return BGPOrigin::EGP;
        case BGPSTREAM_ELEM_BGP_UPDATE_ORIGIN_INCOMPLETE:
            return BGPOrigin::Incomplete;
        default:
            return BGPOrigin::Unknown;
    }
}

BGPPeerState peer_state_from_bgpstream(bgpstream_elem_peerstate_t state) {
    switch (state) {
        case BGPSTREAM_ELEM_PEERSTATE_IDLE:
            return BGPPeerState::Idle;
        case BGPSTREAM_ELEM_PEERSTATE_CONNECT:
            return BGPPeerState::Connect;
        case BGPSTREAM_ELEM_PEERSTATE_ACTIVE:
            return BGPPeerState::Active;
        case BGPSTREAM_ELEM_PEERSTATE_OPENSENT:
            return BGPPeerState::OpenSent;
        case BGPSTREAM_ELEM_PEERSTATE_OPENCONFIRM:
            return BGPPeerState::OpenConfirm;
        case BGPSTREAM_ELEM_PEERSTATE_ESTABLISHED:
            return BGPPeerState::Established;
        case BGPSTREAM_ELEM_PEERSTATE_CLEARING:
            return BGPPeerState::Clearing;
        case BGPSTREAM_ELEM_PEERSTATE_DELETED:
            return BGPPeerState::Deleted;
        case BGPSTREAM_ELEM_PEERSTATE_UNKNOWN:
        default:
            return BGPPeerState::Unknown;
    }
}

std::optional<BGPMessageType> message_type_from_bgpstream(bgpstream_elem_type_t type) {
    switch (type) {
        case BGPSTREAM_ELEM_TYPE_RIB:
            return BGPMessageType::RIB;
        case BGPSTREAM_ELEM_TYPE_ANNOUNCEMENT:
            return BGPMessageType::Announcement;
        case BGPSTREAM_ELEM_TYPE_WITHDRAWAL:
            return BGPMessageType::Withdrawal;
        case BGPSTREAM_ELEM_TYPE_PEERSTATE:
            return BGPMessageType::PeerState;
#if defined(BGPSTREAM_MAJOR_VERSION) && defined(BGPSTREAM_MID_VERSION) && \
    (BGPSTREAM_MAJOR_VERSION > 2 || (BGPSTREAM_MAJOR_VERSION == 2 && BGPSTREAM_MID_VERSION >= 4))
        case BGPSTREAM_ELEM_TYPE_END_OF_RIB:
            return BGPMessageType::EndOfRib;
#endif
        case BGPSTREAM_ELEM_TYPE_UNKNOWN:
        default:
            return std::nullopt;
    }
}

bool message_has_prefix(BGPMessageType type) {
    return type == BGPMessageType::RIB || type == BGPMessageType::Announcement ||
           type == BGPMessageType::Withdrawal;
}

bool message_has_path_attributes(BGPMessageType type) {
    return type == BGPMessageType::RIB || type == BGPMessageType::Announcement;
}

std::string record_source_name(const char *record_value, const std::string &configured_value) {
    const std::string value = record_value == nullptr ? std::string{} : std::string(record_value);
    if (value.empty() || value == "singlefile") {
        return configured_value;
    }
    return value;
}

BGPMessage make_bgp_message(const bgpstream_record_t &record, const bgpstream_elem_t &elem,
                            BGPMessageType message_type, const Config &config, const std::string &source_file,
                            std::uint64_t record_index, std::uint64_t element_index,
                            BGPMessageFields fields) {
    BGPMessage message;

    if (has_message_field(fields, BGPMessageFields::Type)) {
        message.type = message_type;
    }
    if (has_message_field(fields, BGPMessageFields::RecordType)) {
        message.record_type = record_type_from_bgpstream(record.type);
    }
    if (has_message_field(fields, BGPMessageFields::RecordStatus)) {
        message.record_status = record_status_from_bgpstream(record.status);
    }
    if (has_message_field(fields, BGPMessageFields::Timestamp)) {
        message.timestamp = static_cast<std::time_t>(record.time_sec);
        message.timestamp_microseconds = record.time_usec;
    }
    if (has_message_field(fields, BGPMessageFields::ProjectName)) {
        message.project_name = record_source_name(record.project_name, config.project);
    }
    if (has_message_field(fields, BGPMessageFields::CollectorName)) {
        message.collector_name = record_source_name(record.collector_name, config.collector);
    }
    if (has_message_field(fields, BGPMessageFields::RouterName)) {
        message.router_name = record.router_name;
    }
    if (has_message_field(fields, BGPMessageFields::RouterIp)) {
        message.router_ip = address_to_string(record.router_ip);
    }
    if (has_message_field(fields, BGPMessageFields::DumpPosition)) {
        message.dump_position = dump_position_from_bgpstream(record.dump_pos);
    }
    if (has_message_field(fields, BGPMessageFields::DumpTimestamp)) {
        message.dump_timestamp = static_cast<std::time_t>(record.dump_time_sec);
    }
    if (has_message_field(fields, BGPMessageFields::SourceFile)) {
        message.source_file = std::filesystem::absolute(source_file).lexically_normal().string();
    }
    if (has_message_field(fields, BGPMessageFields::RecordIndex)) {
        message.record_index = record_index;
    }
    if (has_message_field(fields, BGPMessageFields::ElementIndex)) {
        message.element_index = element_index;
    }

    if (has_message_field(fields, BGPMessageFields::OriginatedTimestamp)) {
        message.originated_timestamp = static_cast<std::time_t>(elem.orig_time_sec);
        message.originated_timestamp_microseconds = elem.orig_time_usec;
    }
    if (has_message_field(fields, BGPMessageFields::PeerIp)) {
        message.peer_ip = address_to_string(elem.peer_ip);
    }
    if (has_message_field(fields, BGPMessageFields::PeerAsn)) {
        message.peer_asn = elem.peer_asn;
    }
    if (has_message_field(fields, BGPMessageFields::Annotations)) {
        message.annotations.rpki_active = elem.annotations.rpki_active != 0;
        message.annotations.has_rpki_config = elem.annotations.cfg != nullptr;
        message.annotations.timestamp = elem.annotations.timestamp;
    }
    if (has_message_field(fields, BGPMessageFields::Prefix) && message_has_prefix(message_type)) {
        message.prefix = prefix_to_string(elem.prefix);
    }

    if (message_has_path_attributes(message_type)) {
        if (has_message_field(fields, BGPMessageFields::NextHop)) {
            message.next_hop = address_to_string(elem.nexthop);
        }
        if (has_message_field(fields, BGPMessageFields::HasASPath) ||
            has_message_field(fields, BGPMessageFields::ASPathString) ||
            has_message_field(fields, BGPMessageFields::ASPathSegments) ||
            has_message_field(fields, BGPMessageFields::FlattenedAsns) ||
            has_message_field(fields, BGPMessageFields::OriginAsn)) {
            append_requested_as_path(elem.as_path, fields, &message);
        }
        if (has_message_field(fields, BGPMessageFields::HasCommunities)) {
            message.has_communities = elem.communities != nullptr;
        }
        if (has_message_field(fields, BGPMessageFields::Communities)) {
            append_communities(elem.communities, &message);
        }
        if (has_message_field(fields, BGPMessageFields::Origin) && elem.has_origin != 0) {
            message.origin = origin_from_bgpstream(elem.origin);
        }
        if (has_message_field(fields, BGPMessageFields::Med) && elem.has_med != 0) {
            message.med = elem.med;
        }
        if (has_message_field(fields, BGPMessageFields::LocalPref) && elem.has_local_pref != 0) {
            message.local_pref = elem.local_pref;
        }
        if (has_message_field(fields, BGPMessageFields::AtomicAggregate)) {
            message.atomic_aggregate = elem.atomic_aggregate != 0;
        }
        if (has_message_field(fields, BGPMessageFields::Aggregator) && elem.aggregator.has_aggregator != 0) {
            message.aggregator = BGPAggregator{elem.aggregator.aggregator_asn,
                                               address_to_string(elem.aggregator.aggregator_addr)};
        }
    }

    if (has_message_field(fields, BGPMessageFields::PeerStates) && message_type == BGPMessageType::PeerState) {
        message.old_peer_state = peer_state_from_bgpstream(elem.old_state);
        message.new_peer_state = peer_state_from_bgpstream(elem.new_state);
    }
    return message;
}

void record_message_type(BGPMessageType type, MessageTraversalStats *stats) {
    switch (type) {
        case BGPMessageType::RIB:
            ++stats->rib_messages;
            break;
        case BGPMessageType::Announcement:
            ++stats->announcement_messages;
            break;
        case BGPMessageType::Withdrawal:
            ++stats->withdrawal_messages;
            break;
        case BGPMessageType::PeerState:
            ++stats->peer_state_messages;
            break;
        case BGPMessageType::EndOfRib:
            ++stats->end_of_rib_messages;
            break;
        case BGPMessageType::Unknown:
            break;
    }
    ++stats->visited_messages;
}

}  // namespace

MessageTraversalStats traverse_mrt_file(const Config &config, const std::filesystem::path &file_path,
                                        BGPMessageFields fields, std::size_t message_batch_size,
                                        const std::optional<ClosedDateRange> &range,
                                        const MessageBatchHandler &handle_batch) {
    if (message_batch_size == 0) {
        throw std::invalid_argument("message_batch_size must be positive");
    }

    bgpstream_t *stream = bgpstream_create();
    if (stream == nullptr) {
        throw MrtParseFailure("Failed to create BGPStream instance");
    }

    auto destroy_stream = [&stream]() {
        if (stream != nullptr) {
            bgpstream_destroy(stream);
            stream = nullptr;
        }
    };

    try {
        const auto interface_id = bgpstream_get_data_interface_id_by_name(stream, "singlefile");
        if (interface_id == _BGPSTREAM_DATA_INTERFACE_INVALID) {
            throw MrtParseFailure("singlefile data interface is unavailable");
        }
        bgpstream_set_data_interface(stream, interface_id);

        auto *update_file_option =
            bgpstream_get_data_interface_option_by_name(stream, interface_id, "upd-file");
        if (update_file_option == nullptr) {
            throw MrtParseFailure("singlefile/upd-file option is unavailable");
        }
        const std::string file_text = file_path.string();
        if (bgpstream_set_data_interface_option(stream, update_file_option, file_text.c_str()) != 0) {
            throw MrtParseFailure("Failed to set upd-file option for " + file_text);
        }
        if (bgpstream_start(stream) != 0) {
            throw MrtParseFailure("Failed to start BGPStream for " + file_text);
        }

        MessageTraversalStats stats;
        std::vector<BGPMessage> message_batch;
        message_batch.reserve(message_batch_size);
        const std::string source_file =
            has_message_field(fields, BGPMessageFields::SourceFile)
                ? std::filesystem::absolute(file_path).lexically_normal().string()
                : std::string{};

        auto flush_batch = [&]() {
            if (message_batch.empty()) {
                return;
            }
            handle_batch(message_batch);
            message_batch.clear();
        };

        bgpstream_record_t *record = nullptr;
        std::uint64_t record_index = 0;
        while (true) {
            const int record_result = bgpstream_get_next_record(stream, &record);
            if (record_result == 0) {
                break;
            }
            if (record_result < 0) {
                throw MrtParseFailure("Failed to read BGP record stream");
            }
            if (record == nullptr) {
                continue;
            }
            const std::uint64_t current_record_index = record_index++;

            switch (record->status) {
                case BGPSTREAM_RECORD_STATUS_VALID_RECORD:
                    break;
                case BGPSTREAM_RECORD_STATUS_FILTERED_SOURCE:
                case BGPSTREAM_RECORD_STATUS_EMPTY_SOURCE:
                case BGPSTREAM_RECORD_STATUS_OUTSIDE_TIME_INTERVAL:
                    continue;
                default:
                    throw MrtParseFailure(record_status_to_string(record->status));
            }

            bgpstream_elem_t *element = nullptr;
            std::uint64_t element_index = 0;
            while (true) {
                const int element_result = bgpstream_record_get_next_elem(record, &element);
                if (element_result == 0) {
                    break;
                }
                if (element_result < 0) {
                    throw MrtParseFailure("Failed to read BGP element from record");
                }
                if (element == nullptr) {
                    continue;
                }
                const std::uint64_t current_element_index = element_index++;
                const std::optional<BGPMessageType> message_type = message_type_from_bgpstream(element->type);
                if (!message_type.has_value()) {
                    continue;
                }

                const std::time_t timestamp = static_cast<std::time_t>(record->time_sec);
                if (range.has_value() &&
                    !(range->start_epoch <= timestamp && timestamp < range->end_exclusive_epoch)) {
                    continue;
                }

                message_batch.push_back(make_bgp_message(*record, *element, *message_type, config, source_file,
                                                         current_record_index, current_element_index, fields));
                record_message_type(*message_type, &stats);
                if (message_batch.size() >= message_batch_size) {
                    flush_batch();
                }
            }
        }

        flush_batch();
        destroy_stream();
        return stats;
    } catch (...) {
        destroy_stream();
        throw;
    }
}

}  // namespace bgpstream_runner
