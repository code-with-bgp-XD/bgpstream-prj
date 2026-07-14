#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "bgpstream_runner/chunk_engine.h"
#include "bgpstream_runner/common.h"
#include "bgpstream_runner/download_client.h"
#include "bgpstream_runner/parsed_cache.h"
#include "bgpstream_runner/plugin_loader.h"

namespace {

void run_download_and_preparse(const bgpstream_runner::Config &config) {
    const bgpstream_runner::ClosedDateRange range = bgpstream_runner::parse_closed_date_range(config);
    const bgpstream_runner::DownloadClient download_client(config);
    const std::vector<bgpstream_runner::DownloadTarget> targets =
        download_client.collect_targets(range, config.limit);

    if (targets.empty()) {
        throw std::runtime_error("No files matched the requested data source and date range.");
    }

    std::size_t cached_before = 0;
    std::size_t parsed_before = 0;
    for (const auto &target : targets) {
        const std::filesystem::path source_file = std::filesystem::exists(target.local_path)
                                                      ? target.local_path
                                                      : target.destination_path;
        if (std::filesystem::exists(source_file)) {
            ++cached_before;
            if (bgpstream_runner::inspect_parsed_cache(source_file).state ==
                bgpstream_runner::ParsedCacheState::Valid) {
                ++parsed_before;
            }
        }
    }

    std::cout << "download-and-preparse source: " << config.project << '/' << config.collector << '\n'
              << "date_range: " << config.start_date << " through " << config.end_date << " (inclusive)\n"
              << "cache_root: " << std::filesystem::absolute(config.output_dir).string() << '\n'
              << "matched_files: " << targets.size() << '\n'
              << "raw_files_already_cached: " << cached_before << '\n'
              << "parsed_files_already_cached: " << parsed_before << std::endl;

    download_client.download_range(range, config.limit);

    std::vector<std::filesystem::path> missing_files;
    std::vector<std::filesystem::path> source_files;
    source_files.reserve(targets.size());
    std::uint64_t available_bytes = 0;
    for (const auto &target : targets) {
        const std::filesystem::path source_file = std::filesystem::exists(target.local_path)
                                                      ? target.local_path
                                                      : target.destination_path;
        if (std::filesystem::exists(source_file)) {
            available_bytes += bgpstream_runner::safe_file_size(source_file);
            source_files.push_back(source_file);
        } else {
            missing_files.push_back(target.destination_path);
        }
    }

    if (!missing_files.empty()) {
        std::ostringstream message;
        message << "Download incomplete: " << missing_files.size() << '/' << targets.size()
                << " matched files are still missing";
        const std::size_t displayed_count = std::min<std::size_t>(missing_files.size(), 5);
        for (std::size_t index = 0; index < displayed_count; ++index) {
            message << "\n  " << missing_files[index].string();
        }
        if (missing_files.size() > displayed_count) {
            message << "\n  ... and " << (missing_files.size() - displayed_count) << " more";
        }
        throw std::runtime_error(message.str());
    }

    std::cout << "raw download complete: " << targets.size() << " files, "
              << bgpstream_runner::format_bytes(available_bytes)
              << " available locally; original MRT files will be preserved" << std::endl;

    const bgpstream_runner::ParsedCacheBuildSummary parsed =
        bgpstream_runner::ensure_parsed_caches(config, source_files);
    std::cout << "parsed cache complete: " << parsed.source_files << " source files, "
              << parsed.reused_files << " reused, " << parsed.generated_files << " generated, "
              << parsed.generated_messages << " generated messages, "
              << bgpstream_runner::format_bytes(parsed.cache_bytes) << " parsed cache data" << std::endl;
}

}  // namespace

int main(int argc, char **argv) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::unique_ptr<bgpstream_runner::LoadedProcessorPlugin> processor_plugin;
    std::unique_ptr<bgpstream_runner::ChunkEngine> engine;
    try {
        const bgpstream_runner::Config config = bgpstream_runner::parse_args(argc, argv);
        if (config.download_only) {
            run_download_and_preparse(config);
            return 0;
        }
        processor_plugin = std::make_unique<bgpstream_runner::LoadedProcessorPlugin>(config, argv[0]);
        engine = std::make_unique<bgpstream_runner::ChunkEngine>(config, processor_plugin->processor());

        const bgpstream_runner::RangeProcessingStats stats = engine->run();
        if (stats.files_used == 0) {
            throw std::runtime_error("No local files available for statistics.");
        }

        if (config.log_final_summary) {
            engine->print_summary(std::cout, stats, "final cumulative stats");
        }
        engine->write_record_file(stats, "final cumulative stats", "success");
        return 0;
    } catch (const std::exception &exc) {
        if (engine != nullptr) {
            try {
                engine->write_record_file(engine->current_stats(), "aborted cumulative stats", "failed", exc.what());
            } catch (const std::exception &record_exc) {
                std::cerr << record_exc.what() << '\n';
            }
        }
        std::cerr << exc.what() << '\n';
        return 1;
    }
}
