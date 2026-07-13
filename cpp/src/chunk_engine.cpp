extern "C" {
#include <bgpstream.h>
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bgpstream_runner/chunk_engine.h"
#include "bgpstream_runner/common.h"

namespace bgpstream_runner {

namespace {

#ifndef BGPSTREAM_SOURCE_DIR
#define BGPSTREAM_SOURCE_DIR "."
#endif

class ParseFailure : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

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
        throw ParseFailure(std::string("Failed to stringify prefix: ") + std::strerror(errno));
    }
    return std::string(buffer);
}

std::string address_to_string(const bgpstream_ip_addr_t &address) {
    if (address.version == BGPSTREAM_ADDR_VERSION_UNKNOWN) {
        return {};
    }

    char buffer[INET6_ADDRSTRLEN];
    if (bgpstream_addr_ntop(buffer, sizeof(buffer), &address) == nullptr) {
        throw ParseFailure(std::string("Failed to stringify IP address: ") + std::strerror(errno));
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
            throw ParseFailure("Failed to stringify AS path");
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

        while (bgpstream_as_path_seg_t *seg = bgpstream_as_path_get_next_seg(as_path, &iter)) {
            if (needs_segments) {
                ASPathSegment segment;
                segment.type = as_path_segment_type_from_bgpstream(seg->type);
                if (seg->type == BGPSTREAM_AS_PATH_SEG_ASN) {
                    segment.asns.push_back(seg->asn.asn);
                    if (needs_flattened_asns) {
                        message->asns.push_back(seg->asn.asn);
                    }
                } else {
                    segment.asns.reserve(seg->set.asn_cnt);
                    for (std::uint8_t index = 0; index < seg->set.asn_cnt; ++index) {
                        segment.asns.push_back(seg->set.asn[index]);
                        if (needs_flattened_asns) {
                            message->asns.push_back(seg->set.asn[index]);
                        }
                    }
                }
                message->as_path_segments.push_back(std::move(segment));
            } else if (seg->type == BGPSTREAM_AS_PATH_SEG_ASN) {
                message->asns.push_back(seg->asn.asn);
            } else {
                for (std::uint8_t index = 0; index < seg->set.asn_cnt; ++index) {
                    message->asns.push_back(seg->set.asn[index]);
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
            throw ParseFailure("Failed to read community from community set");
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
        message.source_file = source_file;
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
        if (has_message_field(fields, BGPMessageFields::Aggregator) &&
            elem.aggregator.has_aggregator != 0) {
            message.aggregator = BGPAggregator{elem.aggregator.aggregator_asn,
                                               address_to_string(elem.aggregator.aggregator_addr)};
        }
    }

    if (has_message_field(fields, BGPMessageFields::PeerStates) &&
        message_type == BGPMessageType::PeerState) {
        message.old_peer_state = peer_state_from_bgpstream(elem.old_state);
        message.new_peer_state = peer_state_from_bgpstream(elem.new_state);
    }

    return message;
}

std::string format_parse_failure(const std::filesystem::path &file_path, const std::string &reason) {
    return "解析跳过: " + file_path.filename().string() + " | " + reason;
}

std::filesystem::path record_root_dir() {
    const std::filesystem::path configured_root = BGPSTREAM_SOURCE_DIR;
    if (std::filesystem::exists(configured_root)) {
        return configured_root;
    }
    return std::filesystem::current_path();
}

std::string normalized_path_text(const std::filesystem::path &path) {
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::filesystem::path record_log_dir() {
    const std::filesystem::path directory = record_root_dir() / "log";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::runtime_error("Failed to create log directory " + directory.string() + ": " + error.message());
    }
    return directory;
}

std::filesystem::path make_record_file_path() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::tm local_tm{};
    if (localtime_r(&now_time, &local_tm) == nullptr) {
        throw std::runtime_error("Failed to format local time for record file name");
    }

    std::ostringstream base_name;
    base_name << "rcd-" << std::put_time(&local_tm, "%Y%m%d-%H:%M");

    const std::filesystem::path root_dir = record_log_dir();
    const std::string base_text = base_name.str();
    std::filesystem::path candidate = root_dir / base_text;
    if (!std::filesystem::exists(candidate)) {
        return candidate;
    }

    std::ostringstream output;
    for (std::uint32_t suffix = 1;; ++suffix) {
        output.str("");
        output.clear();
        output << base_text << '-' << std::setfill('0') << std::setw(3) << suffix;
        candidate = root_dir / output.str();
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return candidate;
}

}  // namespace

ChunkEngine::ChunkEngine(Config config, MessageProcessor &processor)
    : config_(std::move(config)),
      download_client_(config_),
      processor_(processor),
      processor_requires_strict_chronological_order_(processor.requires_strict_chronological_order()),
      processor_uses_concurrent_message_handling_(processor.supports_concurrent_message_handling() &&
                                                  !processor_requires_strict_chronological_order_),
      message_fields_(processor.required_message_fields() |
                      (processor_requires_strict_chronological_order_ ? BGPMessageFields::Timestamp
                                                                       : BGPMessageFields::None)),
      record_file_path_(make_record_file_path()) {}

RangeProcessingStats ChunkEngine::run() {
    reset_stats();
    last_delivered_message_timestamp_.reset();
    const ClosedDateRange range = parse_closed_date_range(config_);
    const std::vector<ClosedDateRange> chunks = split_range_by_chunks(range, config_.chunk_size, config_.chunk_unit);

    struct PlannedChunk {
        ClosedDateRange range;
        std::string label;
        int limit_override = -1;
        std::vector<DownloadTarget> targets;
    };

    if (config_.log_phase_transitions) {
        std::cout << "plan phase " << format_range_label(range) << std::endl;
    }

    std::vector<PlannedChunk> planned_chunks;
    planned_chunks.reserve(chunks.size());
    std::size_t total_files = 0;
    std::uint64_t total_bytes = 0;
    bool all_file_sizes_known = true;
    int remaining_limit = config_.limit;

    for (const ClosedDateRange &chunk : chunks) {
        if (remaining_limit == 0) {
            break;
        }

        PlannedChunk planned_chunk;
        planned_chunk.range = chunk;
        planned_chunk.label = format_range_label(chunk);
        planned_chunk.limit_override = remaining_limit;
        planned_chunk.targets = download_client_.collect_targets(chunk, remaining_limit);

        // Boundary resources can be traversed once per adjacent chunk because each
        // traversal applies a different time filter, so count every planned target.
        total_files += planned_chunk.targets.size();
        for (const DownloadTarget &target : planned_chunk.targets) {
            std::uint64_t file_size = 0;
            if (std::filesystem::exists(target.local_path)) {
                file_size = safe_file_size(target.local_path);
            } else if (std::filesystem::exists(target.destination_path)) {
                file_size = safe_file_size(target.destination_path);
            } else {
                file_size = target.expected_size_bytes;
            }

            if (file_size == 0) {
                all_file_sizes_known = false;
            } else {
                total_bytes += file_size;
            }
        }

        if (remaining_limit > 0) {
            remaining_limit -= static_cast<int>(planned_chunk.targets.size());
            if (remaining_limit < 0) {
                remaining_limit = 0;
            }
        }
        planned_chunks.push_back(std::move(planned_chunk));
    }

    std::unique_ptr<FileProgressDisplay> progress;
    if (total_files > 0) {
        progress = std::make_unique<FileProgressDisplay>(total_files, all_file_sizes_known ? total_bytes : 0);
    }

    for (const PlannedChunk &planned_chunk : planned_chunks) {
        const ClosedDateRange &chunk = planned_chunk.range;
        const std::string &chunk_label = planned_chunk.label;
        const std::vector<DownloadTarget> &targets = planned_chunk.targets;

        increment_chunk_count();

        try {
            if (config_.log_phase_transitions) {
                std::cout << "download phase " << chunk_label << std::endl;
            }
            if (targets.empty()) {
                const RangeProcessingStats stats = current_stats();
                if (config_.log_chunk_summary) {
                    print_summary(std::cout, stats, "current cumulative stats after chunk " + chunk_label);
                }
                write_record_file(stats, "current cumulative stats after chunk " + chunk_label, "chunk-complete");
                continue;
            }

            std::vector<std::filesystem::path> existing_files = existing_target_files(targets);
            if (existing_files.size() != targets.size()) {
                evict_cache_if_needed(targets);
                download_client_.download_range(chunk, planned_chunk.limit_override, false);
                existing_files = existing_target_files(targets);
            } else if (config_.log_phase_transitions) {
                std::cout << "skip remote download " << chunk_label
                          << " because all target files are already cached locally" << std::endl;
            }

            const std::size_t unavailable_files = targets.size() - existing_files.size();
            if (unavailable_files > 0) {
                progress->mark_batch_completed(unavailable_files, 0);
            }

            if (!existing_files.empty()) {
                if (config_.log_phase_transitions) {
                    std::cout << "process phase " << chunk_label << std::endl;
                }
                process_files(existing_files, chunk, *progress);
            } else {
                if (config_.log_phase_transitions) {
                    std::cout << "skip process phase " << chunk_label << " because no local files are available"
                              << std::endl;
                }
            }

            const RangeProcessingStats stats = current_stats();
            if (config_.log_chunk_summary) {
                print_summary(std::cout, stats, "current cumulative stats after chunk " + chunk_label);
            }
            write_record_file(stats, "current cumulative stats after chunk " + chunk_label, "chunk-complete");
        } catch (...) {
            throw;
        }
    }

    if (progress != nullptr) {
        progress->finish();
    }
    processor_.finalize();
    return current_stats();
}

RangeProcessingStats ChunkEngine::current_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void ChunkEngine::print_summary(std::ostream &out, const RangeProcessingStats &stats, std::string_view title) const {
    const auto data_dir = config_.output_dir / config_.project / config_.collector / "updates";

    out << title << '\n';
    out << "processor: " << processor_.name() << '\n';
    out << "start_date: " << config_.start_date << '\n';
    out << "end_date: " << config_.end_date << '\n';
    out << "collector: " << config_.collector << '\n';
    out << "data_dir: " << std::filesystem::absolute(data_dir).string() << '\n';
    out << "download_workers: " << config_.download_workers << '\n';
    out << "parser_workers: " << config_.parser_workers << '\n';
    out << "processor_concurrent_message_handling: "
        << (processor_uses_concurrent_message_handling_ ? "true" : "false") << '\n';
    out << "processor_strict_chronological_order: "
        << (processor_requires_strict_chronological_order_ ? "true" : "false") << '\n';
    out << "message_batch_size: " << config_.message_batch_size << '\n';
    out << "chunk_size: " << config_.chunk_size << '\n';
    out << "chunk_unit: " << chunk_unit_to_string(config_.chunk_unit) << '\n';
    out << "max_cache_size_gb: " << config_.max_cache_size_gb << '\n';
    out << "processed_chunks: " << stats.chunk_count << '\n';
    out << "files_used: " << stats.files_used << '\n';
    out << "visited_messages: " << stats.visited_messages << '\n';
    out << "rib_messages: " << stats.rib_messages << '\n';
    out << "announcement_messages: " << stats.announcement_messages << '\n';
    out << "withdrawal_messages: " << stats.withdrawal_messages << '\n';
    out << "peer_state_messages: " << stats.peer_state_messages << '\n';
    out << "end_of_rib_messages: " << stats.end_of_rib_messages << '\n';
    out << "skipped_parse_files: " << stats.skipped_parse_files << '\n';
    out << "============================= plugin output =============================" << '\n';
    processor_.print_summary(out);
    out << "=========================== plugin output end ===========================" << '\n';
    out << std::flush;
}

std::filesystem::path ChunkEngine::write_record_file(const RangeProcessingStats &stats, std::string_view title,
                                                     std::string_view run_status,
                                                     std::string_view error_message) const {
    std::lock_guard<std::mutex> lock(record_file_mutex_);
    const bool already_exists = std::filesystem::exists(record_file_path_);
    const bool has_content = already_exists && safe_file_size(record_file_path_) > 0;

    std::ofstream output(record_file_path_, std::ios::app);
    if (!output) {
        throw std::runtime_error("Failed to open record file: " + record_file_path_.string());
    }

    if (has_content) {
        output << '\n';
    }
    output << "============================================================\n";

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    output << "record_generated_at: " << format_utc_timestamp(now) << '\n';
    output << "run_status: " << run_status << '\n';
    if (!error_message.empty()) {
        output << "error_message: " << error_message << '\n';
    }
    print_summary(output, stats, title);
    return record_file_path_;
}

void ChunkEngine::process_files(const std::vector<std::filesystem::path> &files, const ClosedDateRange &chunk,
                                FileProgressDisplay &progress) {
    if (files.empty()) {
        return;
    }

    // `files` preserves the resource list's initial-time ordering. A single
    // worker keeps that order across files when strict delivery is requested.
    const std::size_t worker_count = processor_requires_strict_chronological_order_
                                         ? 1
                                         : std::min<std::size_t>(files.size(),
                                                                 static_cast<std::size_t>(config_.parser_workers));

    try {
        std::atomic<std::size_t> next_file_index{0};
        std::mutex processor_mutex;
        std::mutex *const processor_mutex_ptr =
            processor_uses_concurrent_message_handling_ ? nullptr : &processor_mutex;
        std::mutex parse_failures_mutex;
        std::mutex fatal_error_mutex;
        std::exception_ptr fatal_error;
        std::vector<std::string> parse_failures;
        std::vector<std::thread> workers;
        workers.reserve(worker_count);

        for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
            workers.emplace_back([&, worker_index]() {
                (void)worker_index;
                try {
                    while (true) {
                        const std::size_t file_index = next_file_index.fetch_add(1, std::memory_order_relaxed);
                        if (file_index >= files.size()) {
                            break;
                        }

                        const auto &file_path = files[file_index];
                        try {
                            const FileTraversalStats file_stats =
                                traverse_single_file(file_path, chunk, processor_mutex_ptr);
                            record_processed_file(file_stats);
                        } catch (const ParseFailure &exc) {
                            record_skipped_parse_file();
                            std::lock_guard<std::mutex> lock(parse_failures_mutex);
                            parse_failures.push_back(format_parse_failure(file_path, exc.what()));
                        }

                        progress.mark_batch_completed(1, safe_file_size(file_path));
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(fatal_error_mutex);
                    if (fatal_error == nullptr) {
                        fatal_error = std::current_exception();
                    }
                }
            });
        }

        for (auto &worker : workers) {
            worker.join();
        }

        if (fatal_error != nullptr) {
            std::rethrow_exception(fatal_error);
        }

        for (const auto &message : parse_failures) {
            std::cerr << message << '\n';
        }
    } catch (...) {
        throw;
    }
}

ChunkEngine::FileTraversalStats ChunkEngine::traverse_single_file(const std::filesystem::path &file_path,
                                                                  const ClosedDateRange &chunk,
                                                                  std::mutex *processor_mutex) {
    bgpstream_t *stream = bgpstream_create();
    if (stream == nullptr) {
        throw ParseFailure("Failed to create BGPStream instance");
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
            throw ParseFailure("singlefile data interface is unavailable");
        }

        bgpstream_set_data_interface(stream, interface_id);

        auto *upd_file_option = bgpstream_get_data_interface_option_by_name(stream, interface_id, "upd-file");
        if (upd_file_option == nullptr) {
            throw ParseFailure("singlefile/upd-file option is unavailable");
        }

        if (bgpstream_set_data_interface_option(stream, upd_file_option, file_path.string().c_str()) != 0) {
            throw ParseFailure("Failed to set upd-file option for " + file_path.string());
        }

        if (bgpstream_start(stream) != 0) {
            throw ParseFailure("Failed to start BGPStream for " + file_path.string());
        }

        FileTraversalStats stats;
        std::vector<BGPMessage> message_batch;
        message_batch.reserve(static_cast<std::size_t>(config_.message_batch_size));
        const std::string source_file = has_message_field(message_fields_, BGPMessageFields::SourceFile)
                                            ? normalized_path_text(file_path)
                                            : std::string{};

        auto flush_batch = [&]() {
            if (message_batch.empty()) {
                return;
            }
            dispatch_message_batch(message_batch, processor_mutex);
        };

        bgpstream_record_t *record = nullptr;
        std::uint64_t record_index = 0;
        while (true) {
            const int record_rc = bgpstream_get_next_record(stream, &record);
            if (record_rc == 0) {
                break;
            }
            if (record_rc < 0) {
                throw ParseFailure("Failed to read BGP record stream");
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
                    throw ParseFailure(record_status_to_string(record->status));
            }

            bgpstream_elem_t *elem = nullptr;
            std::uint64_t element_index = 0;
            while (true) {
                const int elem_rc = bgpstream_record_get_next_elem(record, &elem);
                if (elem_rc == 0) {
                    break;
                }
                if (elem_rc < 0) {
                    throw ParseFailure("Failed to read BGP element from record");
                }
                if (elem == nullptr) {
                    continue;
                }
                const std::uint64_t current_element_index = element_index++;

                const std::optional<BGPMessageType> message_type = message_type_from_bgpstream(elem->type);
                if (!message_type.has_value()) {
                    continue;
                }

                const std::time_t timestamp = static_cast<std::time_t>(record->time_sec);
                if (!(chunk.start_epoch <= timestamp && timestamp < chunk.end_exclusive_epoch)) {
                    continue;
                }

                BGPMessage message = make_bgp_message(*record, *elem, *message_type, config_, source_file,
                                                      current_record_index, current_element_index, message_fields_);
                switch (*message_type) {
                    case BGPMessageType::RIB:
                        stats.rib_messages += 1;
                        break;
                    case BGPMessageType::Announcement:
                        stats.announcement_messages += 1;
                        break;
                    case BGPMessageType::Withdrawal:
                        stats.withdrawal_messages += 1;
                        break;
                    case BGPMessageType::PeerState:
                        stats.peer_state_messages += 1;
                        break;
                    case BGPMessageType::EndOfRib:
                        stats.end_of_rib_messages += 1;
                        break;
                    case BGPMessageType::Unknown:
                        break;
                }
                stats.visited_messages += 1;

                message_batch.push_back(std::move(message));
                if (message_batch.size() >= static_cast<std::size_t>(config_.message_batch_size)) {
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

void ChunkEngine::dispatch_message_batch(std::vector<BGPMessage> &messages, std::mutex *processor_mutex) {
    if (messages.empty()) {
        return;
    }

    std::optional<MessageTimestamp> delivered_timestamp;
    if (processor_requires_strict_chronological_order_) {
        const auto timestamp_of = [](const BGPMessage &message) {
            return MessageTimestamp{message.timestamp, message.timestamp_microseconds};
        };
        std::stable_sort(messages.begin(), messages.end(), [&](const BGPMessage &left, const BGPMessage &right) {
            return timestamp_of(left) < timestamp_of(right);
        });

        const MessageTimestamp first_timestamp = timestamp_of(messages.front());
        if (last_delivered_message_timestamp_.has_value() && first_timestamp < *last_delivered_message_timestamp_) {
            std::ostringstream error;
            error << "Strict chronological message order cannot be guaranteed: " << first_timestamp.first << '.'
                  << std::setfill('0') << std::setw(6) << first_timestamp.second;
            if (has_message_field(message_fields_, BGPMessageFields::SourceFile)) {
                error << " from " << messages.front().source_file;
            }
            error << " follows " << last_delivered_message_timestamp_->first << '.'
                  << std::setfill('0') << std::setw(6) << last_delivered_message_timestamp_->second;
            throw std::runtime_error(error.str());
        }
        delivered_timestamp = timestamp_of(messages.back());
    }

    if (processor_mutex == nullptr) {
        processor_.handle_messages(messages);
    } else {
        std::lock_guard<std::mutex> lock(*processor_mutex);
        processor_.handle_messages(messages);
    }

    if (delivered_timestamp.has_value()) {
        last_delivered_message_timestamp_ = delivered_timestamp;
    }
    messages.clear();
}

void ChunkEngine::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = RangeProcessingStats{};
}

void ChunkEngine::increment_chunk_count() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.chunk_count += 1;
}

void ChunkEngine::record_processed_file(const FileTraversalStats &file_stats) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.files_used += 1;
    stats_.visited_messages += file_stats.visited_messages;
    stats_.rib_messages += file_stats.rib_messages;
    stats_.announcement_messages += file_stats.announcement_messages;
    stats_.withdrawal_messages += file_stats.withdrawal_messages;
    stats_.peer_state_messages += file_stats.peer_state_messages;
    stats_.end_of_rib_messages += file_stats.end_of_rib_messages;
}

void ChunkEngine::record_skipped_parse_file() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.files_used += 1;
    stats_.skipped_parse_files += 1;
}

std::vector<std::filesystem::path> ChunkEngine::existing_target_files(const std::vector<DownloadTarget> &targets) const {
    std::vector<std::filesystem::path> existing_files;
    existing_files.reserve(targets.size());
    for (const auto &target : targets) {
        if (std::filesystem::exists(target.local_path)) {
            existing_files.push_back(target.local_path);
        } else if (target.local_path != target.destination_path &&
                   std::filesystem::exists(target.destination_path)) {
            existing_files.push_back(target.destination_path);
        }
    }
    return existing_files;
}

std::uint64_t ChunkEngine::cache_size_bytes() const {
    if (!std::filesystem::exists(config_.output_dir)) {
        return 0;
    }

    std::uint64_t total_bytes = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(config_.output_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        total_bytes += safe_file_size(entry.path());
    }
    return total_bytes;
}

ChunkEngine::PlannedDownloadEstimate ChunkEngine::planned_download_bytes(
    const std::vector<DownloadTarget> &targets) const {
    PlannedDownloadEstimate estimate;
    for (const auto &target : targets) {
        if (std::filesystem::exists(target.local_path)) {
            continue;
        }

        if (target.expected_size_bytes == 0) {
            estimate.all_sizes_known = false;
            continue;
        }

        const std::filesystem::path partial_path =
            target.destination_path.parent_path() / (target.destination_path.filename().string() + ".part");
        const std::uint64_t partial_bytes = safe_file_size(partial_path);
        if (target.expected_size_bytes > partial_bytes) {
            estimate.additional_bytes += target.expected_size_bytes - partial_bytes;
        }
    }
    return estimate;
}

void ChunkEngine::evict_cache_if_needed(const std::vector<DownloadTarget> &targets) const {
    const std::uint64_t max_cache_bytes =
        static_cast<std::uint64_t>(config_.max_cache_size_gb * 1024.0 * 1024.0 * 1024.0);
    const PlannedDownloadEstimate estimate = planned_download_bytes(targets);
    const std::uint64_t current_cache_bytes = cache_size_bytes();
    if (estimate.all_sizes_known && current_cache_bytes + estimate.additional_bytes <= max_cache_bytes) {
        return;
    }
    if (!estimate.all_sizes_known && current_cache_bytes <= max_cache_bytes) {
        return;
    }

    std::unordered_set<std::string> protected_paths;
    protected_paths.reserve(targets.size() * 3);
    for (const auto &target : targets) {
        protected_paths.insert(normalized_path_text(target.local_path));
        protected_paths.insert(normalized_path_text(target.destination_path));
        protected_paths.insert(
            normalized_path_text(target.destination_path.parent_path() / (target.destination_path.filename().string() + ".part")));
    }

    struct CacheEntry {
        std::filesystem::path path;
        std::uint64_t size = 0;
        std::filesystem::file_time_type last_write_time;
    };

    std::vector<CacheEntry> entries;
    entries.reserve(1024);
    std::uint64_t total_bytes = 0;
    if (std::filesystem::exists(config_.output_dir)) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(config_.output_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            const std::filesystem::path path = entry.path();
            const std::string canonical_path = normalized_path_text(path);
            const std::uint64_t file_size = safe_file_size(path);
            total_bytes += file_size;

            if (protected_paths.find(canonical_path) != protected_paths.end()) {
                continue;
            }

            entries.push_back(CacheEntry{path, file_size, entry.last_write_time()});
        }
    }

    std::uint64_t pressure_bytes = total_bytes;
    if (estimate.all_sizes_known) {
        pressure_bytes += estimate.additional_bytes;
    }
    if (pressure_bytes <= max_cache_bytes) {
        return;
    }

    std::sort(entries.begin(), entries.end(),
              [](const CacheEntry &lhs, const CacheEntry &rhs) { return lhs.last_write_time < rhs.last_write_time; });

    std::size_t removed_files = 0;
    std::uint64_t removed_bytes = 0;
    for (const auto &entry : entries) {
        if (pressure_bytes <= max_cache_bytes) {
            break;
        }

        std::error_code error;
        std::filesystem::remove(entry.path, error);
        if (error) {
            throw std::runtime_error("Failed to evict cache file " + entry.path.string() + ": " + error.message());
        }
        total_bytes -= entry.size;
        if (pressure_bytes >= entry.size) {
            pressure_bytes -= entry.size;
        } else {
            pressure_bytes = 0;
        }
        removed_bytes += entry.size;
        removed_files += 1;
    }

    if (pressure_bytes > max_cache_bytes) {
        if (estimate.all_sizes_known) {
            throw std::runtime_error("Cache limit " + format_bytes(max_cache_bytes) +
                                     " is too small for the current chunk. Protected files plus pending downloads need " +
                                     format_bytes(pressure_bytes) + " under " + config_.output_dir.string());
        }
        throw std::runtime_error("Cache limit " + format_bytes(max_cache_bytes) +
                                 " is already exceeded by protected files under " + config_.output_dir.string() +
                                 ", and pending download sizes are unknown.");
    }

    if (config_.log_phase_transitions && removed_files > 0) {
        std::cout << "cache eviction removed " << removed_files << " files, freed " << format_bytes(removed_bytes);
        if (estimate.all_sizes_known) {
            std::cout << ", projected cache after download approximately " << format_bytes(pressure_bytes);
        } else {
            std::cout << ", remaining cache approximately " << format_bytes(total_bytes)
                      << " before pending downloads with unknown size";
        }
        std::cout << std::endl;
    }
}

}  // namespace bgpstream_runner
