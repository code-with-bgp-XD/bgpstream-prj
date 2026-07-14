#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "bgpstream_runner/parsed_cache.h"

namespace {

using namespace bgpstream_runner;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path make_temp_directory(const std::string &prefix) {
    const std::string pattern =
        (std::filesystem::temp_directory_path() / (prefix + "-XXXXXX")).string();
    std::vector<char> writable_pattern(pattern.begin(), pattern.end());
    writable_pattern.push_back('\0');
    char *const created = ::mkdtemp(writable_pattern.data());
    if (created == nullptr) {
        throw std::system_error(errno, std::generic_category(), "mkdtemp failed");
    }
    return created;
}

BGPMessage make_message(BGPMessageType type, std::time_t timestamp, std::string prefix) {
    BGPMessage message;
    message.type = type;
    message.record_type = BGPRecordType::Update;
    message.record_status = BGPRecordStatus::Valid;
    message.timestamp = timestamp;
    message.timestamp_microseconds = 123456;
    message.project_name = "routeviews";
    message.collector_name = "route-views.test";
    message.router_name = "router-a";
    message.router_ip = "192.0.2.1";
    message.dump_position = BGPDumpPosition::Middle;
    message.dump_timestamp = timestamp - 1;
    message.source_file = "this value must not be persisted";
    message.record_index = 17;
    message.element_index = 3;
    message.originated_timestamp = timestamp - 2;
    message.originated_timestamp_microseconds = 42;
    message.peer_ip = "2001:db8::1";
    message.peer_asn = 64500;
    message.prefix = std::move(prefix);
    message.next_hop = "2001:db8::2";
    message.has_as_path = true;
    message.as_path = "64500 {64501,64502}";
    message.as_path_segments = {
        ASPathSegment{ASPathSegmentType::ASN, {64500}},
        ASPathSegment{ASPathSegmentType::Set, {64501, 64502}},
    };
    message.asns = {64500, 64501, 64502};
    message.origin_asn = 64502;
    message.has_communities = true;
    message.communities = {{64500, 100}, {64500, 200}};
    message.origin = BGPOrigin::IGP;
    message.med = 50;
    message.local_pref = 100;
    message.atomic_aggregate = true;
    message.aggregator = BGPAggregator{64500, "192.0.2.9"};
    message.old_peer_state = BGPPeerState::Idle;
    message.new_peer_state = BGPPeerState::Established;
    message.annotations = BGPAnnotations{true, true, 99};
    return message;
}

void check_full_message(const BGPMessage &actual, const BGPMessage &expected,
                        const std::filesystem::path &source_file) {
    require(actual.type == expected.type, "type was not preserved");
    require(actual.record_type == expected.record_type, "record type was not preserved");
    require(actual.record_status == expected.record_status, "record status was not preserved");
    require(actual.timestamp == expected.timestamp &&
                actual.timestamp_microseconds == expected.timestamp_microseconds,
            "timestamp was not preserved");
    require(actual.project_name == expected.project_name && actual.collector_name == expected.collector_name,
            "source names were not preserved");
    require(actual.router_name == expected.router_name && actual.router_ip == expected.router_ip,
            "router identity was not preserved");
    require(actual.dump_position == expected.dump_position && actual.dump_timestamp == expected.dump_timestamp,
            "dump metadata was not preserved");
    require(actual.source_file == std::filesystem::absolute(source_file).lexically_normal().string(),
            "source_file was not restored from the current MRT path");
    require(actual.record_index == expected.record_index && actual.element_index == expected.element_index,
            "record identity was not preserved");
    require(actual.originated_timestamp == expected.originated_timestamp &&
                actual.originated_timestamp_microseconds == expected.originated_timestamp_microseconds,
            "originated timestamp was not preserved");
    require(actual.peer_ip == expected.peer_ip && actual.peer_asn == expected.peer_asn,
            "peer identity was not preserved");
    require(actual.prefix == expected.prefix && actual.next_hop == expected.next_hop,
            "NLRI fields were not preserved");
    require(actual.has_as_path == expected.has_as_path && actual.as_path == expected.as_path,
            "AS path presence/string was not preserved");
    require(actual.as_path_segments.size() == 2 && actual.as_path_segments[1].asns == expected.as_path_segments[1].asns,
            "structured AS path was not preserved");
    require(actual.asns == expected.asns && actual.origin_asn == expected.origin_asn,
            "derived AS path views were not preserved");
    require(actual.has_communities == expected.has_communities && actual.communities.size() == 2 &&
                actual.communities[1].value == 200,
            "communities were not preserved");
    require(actual.origin == expected.origin && actual.med == expected.med &&
                actual.local_pref == expected.local_pref,
            "route attributes were not preserved");
    require(actual.atomic_aggregate == expected.atomic_aggregate && actual.aggregator.has_value() &&
                actual.aggregator->address == expected.aggregator->address,
            "aggregate attributes were not preserved");
    require(actual.old_peer_state == expected.old_peer_state && actual.new_peer_state == expected.new_peer_state,
            "peer states were not preserved");
    require(actual.annotations.rpki_active && actual.annotations.has_rpki_config &&
                actual.annotations.timestamp == 99,
            "annotations were not preserved");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 2) {
        const std::filesystem::path real_test_root =
            make_temp_directory("bgpstream-real-cache-test");
        try {
            const std::filesystem::path input_file = argv[1];
            const std::filesystem::path source_file = real_test_root / input_file.filename();
            std::filesystem::copy_file(input_file, source_file,
                                       std::filesystem::copy_options::overwrite_existing);

            Config config;
            config.project = "routeviews";
            config.collector = "route-views.sg";
            config.parser_workers = 2;
            config.message_batch_size = 512;
            const ParsedCacheBuildSummary generated = ensure_parsed_caches(config, {source_file}, false);
            require(generated.generated_files == 1 && generated.reused_files == 0,
                    "real MRT was not parsed into a new cache");
            const ParsedCacheBuildSummary reused = ensure_parsed_caches(config, {source_file}, false);
            require(reused.generated_files == 0 && reused.reused_files == 1,
                    "valid real MRT cache was not reused");

            const std::filesystem::path cache_file = parsed_cache_path(source_file);
            std::filesystem::resize_file(cache_file, std::filesystem::file_size(cache_file) - 8);
            const ParsedCacheBuildSummary regenerated =
                ensure_parsed_caches(config, {source_file}, false);
            require(regenerated.generated_files == 1 && regenerated.reused_files == 0,
                    "truncated real MRT cache was not regenerated");

            const MessageTraversalStats read = read_parsed_cache(
                source_file, kAllBGPMessageFields,
                static_cast<std::size_t>(config.message_batch_size), std::nullopt,
                [](std::vector<BGPMessage> &) {});
            require(regenerated.generated_messages == read.visited_messages,
                    "real MRT cache counts did not round-trip");
            std::cout << "messages=" << read.visited_messages << '\n';
            std::filesystem::remove_all(real_test_root);
            return 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            std::error_code cleanup_error;
            std::filesystem::remove_all(real_test_root, cleanup_error);
            return 1;
        }
    }

    const std::filesystem::path test_root =
        make_temp_directory("bgpstream-parsed-cache-test");
    const std::filesystem::path source_file = test_root / "updates.test.bz2";

    try {
        {
            std::ofstream source(source_file, std::ios::binary);
            source << "source fingerprint";
        }

        const std::vector<BGPMessage> expected{
            make_message(BGPMessageType::Announcement, 100, "203.0.113.0/24"),
            make_message(BGPMessageType::Withdrawal, 200, "2001:db8:1::/48"),
        };
        ParsedCacheWriter writer(source_file);
        writer.append(expected);
        writer.finalize();

        const ParsedCacheInspection inspection = inspect_parsed_cache(source_file);
        require(inspection.state == ParsedCacheState::Valid, "new cache did not validate: " + inspection.reason);

        std::vector<BGPMessage> decoded;
        const MessageTraversalStats full_stats = read_parsed_cache(
            source_file, kAllBGPMessageFields, 1, std::nullopt,
            [&](std::vector<BGPMessage> &batch) { decoded.insert(decoded.end(), batch.begin(), batch.end()); });
        require(full_stats.visited_messages == 2 && full_stats.announcement_messages == 1 &&
                    full_stats.withdrawal_messages == 1,
                "full cache statistics were incorrect");
        require(decoded.size() == expected.size(), "full cache read returned the wrong number of messages");
        check_full_message(decoded.front(), expected.front(), source_file);
        check_full_message(decoded.back(), expected.back(), source_file);

        decoded.clear();
        const ClosedDateRange second_only{150, 300};
        const BGPMessageFields projected_fields = BGPMessageFields::Type | BGPMessageFields::Prefix;
        const MessageTraversalStats projected_stats = read_parsed_cache(
            source_file, projected_fields, 16, second_only,
            [&](std::vector<BGPMessage> &batch) { decoded.insert(decoded.end(), batch.begin(), batch.end()); });
        require(projected_stats.visited_messages == 1 && projected_stats.withdrawal_messages == 1,
                "time-range filtering returned incorrect statistics");
        require(decoded.size() == 1 && decoded.front().type == BGPMessageType::Withdrawal &&
                    decoded.front().prefix == expected.back().prefix,
                "projected cache read returned incorrect data");
        require(decoded.front().peer_ip.empty() && decoded.front().as_path_segments.empty(),
                "unrequested fields were materialized");

        const std::filesystem::path cache_file = parsed_cache_path(source_file);
        const std::uint64_t complete_cache_size = std::filesystem::file_size(cache_file);
        std::filesystem::resize_file(cache_file, complete_cache_size - 8);
        require(inspect_parsed_cache(source_file).state == ParsedCacheState::Invalid,
                "truncated parsed cache passed validation");

        ParsedCacheWriter replacement_writer(source_file);
        replacement_writer.append(expected);
        replacement_writer.finalize();
        require(inspect_parsed_cache(source_file).state == ParsedCacheState::Valid,
                "parsed cache could not be atomically replaced");

        {
            std::ofstream source(source_file, std::ios::binary | std::ios::app);
            source << '!';
        }
        require(inspect_parsed_cache(source_file).state == ParsedCacheState::Invalid,
                "source fingerprint change did not invalidate the parsed cache");

        std::filesystem::remove_all(test_root);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(test_root, cleanup_error);
        return 1;
    }
}
