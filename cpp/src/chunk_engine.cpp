#include <algorithm>
#include <atomic>
#include <chrono>
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
#include <utility>
#include <vector>

#include "bgpstream_runner/chunk_engine.h"
#include "bgpstream_runner/common.h"
#include "bgpstream_runner/parsed_cache.h"

namespace bgpstream_runner {

namespace {

#ifndef BGPSTREAM_SOURCE_DIR
#define BGPSTREAM_SOURCE_DIR "."
#endif

std::filesystem::path record_root_dir() {
    const std::filesystem::path configured_root = BGPSTREAM_SOURCE_DIR;
    if (std::filesystem::exists(configured_root)) {
        return configured_root;
    }
    return std::filesystem::current_path();
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

std::optional<std::filesystem::path> local_source_path(const DownloadTarget &target) {
    if (std::filesystem::exists(target.local_path)) {
        return target.local_path;
    }
    if (std::filesystem::exists(target.destination_path)) {
        return target.destination_path;
    }
    return std::nullopt;
}

std::vector<std::filesystem::path> require_local_source_files(
    const std::vector<DownloadTarget> &targets) {
    std::vector<std::filesystem::path> source_files;
    source_files.reserve(targets.size());
    for (const DownloadTarget &target : targets) {
        const std::optional<std::filesystem::path> source_file = local_source_path(target);
        if (!source_file.has_value()) {
            throw std::runtime_error("Required source MRT is missing before chunk processing: " +
                                     target.destination_path.string());
        }
        source_files.push_back(*source_file);
    }
    return source_files;
}

ParsedCacheInspection inspect_complete_parsed_cache(const std::filesystem::path &source_file) {
    ParsedCacheInspection inspection;
    inspection.cache_path = parsed_cache_path(source_file);
    if (!std::filesystem::exists(inspection.cache_path)) {
        inspection.state = ParsedCacheState::Missing;
        inspection.reason = "parsed cache is missing";
        return inspection;
    }

    try {
        const std::optional<ClosedDateRange> empty_range = ClosedDateRange{};
        (void)read_parsed_cache(source_file, BGPMessageFields::None, 1, empty_range,
                                [](std::vector<BGPMessage> &) {});
        inspection.state = ParsedCacheState::Valid;
    } catch (const std::exception &error) {
        inspection.state = ParsedCacheState::Invalid;
        inspection.reason = error.what();
    }
    return inspection;
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
    const bool recover_chunk_data =
        config_.chunk_data_failure_action == ChunkDataFailureAction::DownloadAndParse;
    bool all_input_sizes_known = !recover_chunk_data;
    int remaining_limit = config_.limit;
    FileProgressDisplay plan_progress(chunks.size(), 0, "analysis-plan", "chunks", false);
    std::size_t completed_plan_chunks = 0;

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
            const std::optional<std::filesystem::path> source_file = local_source_path(target);
            if (!source_file.has_value()) {
                all_input_sizes_known = false;
                if (recover_chunk_data) {
                    continue;
                }
                throw std::runtime_error("Required source MRT is missing: " +
                                         target.destination_path.string() +
                                         ". Set analysis.chunk_data_failure_action to "
                                         "'download_and_parse' to recover the chunk automatically.");
            }
            const std::filesystem::path cache_file = parsed_cache_path(*source_file);
            if (std::filesystem::exists(cache_file)) {
                total_bytes += safe_file_size(cache_file);
            } else {
                all_input_sizes_known = false;
            }
        }

        if (remaining_limit > 0) {
            remaining_limit -= static_cast<int>(planned_chunk.targets.size());
            if (remaining_limit < 0) {
                remaining_limit = 0;
            }
        }
        planned_chunks.push_back(std::move(planned_chunk));
        plan_progress.mark_batch_completed(1, 0);
        ++completed_plan_chunks;
    }
    if (completed_plan_chunks < chunks.size()) {
        plan_progress.mark_batch_completed(chunks.size() - completed_plan_chunks, 0);
    }
    plan_progress.finish();

    std::unique_ptr<FileProgressDisplay> progress;
    if (total_files > 0) {
        const bool may_use_realtime_parser =
            config_.parse_on_cache_miss ||
            (recover_chunk_data && !config_.persist_realtime_parsed_cache);
        progress = std::make_unique<FileProgressDisplay>(
            total_files, total_bytes, may_use_realtime_parser ? "analysis-input" : "parsed-cache",
            "files", all_input_sizes_known);
    }

    for (const PlannedChunk &planned_chunk : planned_chunks) {
        const ClosedDateRange &chunk = planned_chunk.range;
        const std::string &chunk_label = planned_chunk.label;
        const std::vector<DownloadTarget> &targets = planned_chunk.targets;

        increment_chunk_count();

        if (recover_chunk_data) {
            const std::size_t missing_source_files = static_cast<std::size_t>(
                std::count_if(targets.begin(), targets.end(), [](const DownloadTarget &target) {
                    return !local_source_path(target).has_value();
                }));
            if (missing_source_files > 0) {
                if (config_.log_phase_transitions) {
                    std::cout << "recover chunk download phase " << chunk_label << ": "
                              << missing_source_files << " source file(s) missing" << std::endl;
                }
                download_client_.download_range(chunk, planned_chunk.limit_override, false);
            }
        }

        const std::vector<std::filesystem::path> source_files = require_local_source_files(targets);

        SourceFileSet force_realtime_parse_files;
        if (recover_chunk_data) {
            std::vector<std::filesystem::path> unavailable_caches;
            for (const std::filesystem::path &source_file : source_files) {
                if (inspect_complete_parsed_cache(source_file).state != ParsedCacheState::Valid) {
                    unavailable_caches.push_back(source_file);
                }
            }
            if (!unavailable_caches.empty()) {
                if (config_.log_phase_transitions) {
                    std::cout << "recover chunk parse phase " << chunk_label << ": "
                              << unavailable_caches.size() << " cache file(s) missing or invalid; "
                              << (config_.persist_realtime_parsed_cache
                                      ? "rebuilding permanent caches"
                                      : "parsing source MRTs without persisting new caches")
                              << std::endl;
                }
                if (config_.persist_realtime_parsed_cache) {
                    const ParsedCacheBuildSummary repairs =
                        ensure_parsed_caches(config_, unavailable_caches, false, true);
                    record_realtime_parsed_files(repairs.generated_files);
                } else {
                    force_realtime_parse_files.insert(unavailable_caches.begin(),
                                                      unavailable_caches.end());
                }
            }
        }

        if (source_files.empty()) {
            const RangeProcessingStats stats = current_stats();
            if (config_.log_chunk_summary) {
                print_summary(std::cout, stats, "current cumulative stats after chunk " + chunk_label);
            }
            write_record_file(stats, "current cumulative stats after chunk " + chunk_label, "chunk-complete");
            continue;
        }

        if (config_.log_phase_transitions) {
            const bool uses_realtime_recovery = !force_realtime_parse_files.empty();
            std::cout << "process "
                      << (config_.parse_on_cache_miss || uses_realtime_recovery
                              ? "analysis-input"
                              : "parsed-cache")
                      << " phase " << chunk_label << std::endl;
        }
        process_files(source_files, chunk, *progress, force_realtime_parse_files);

        const RangeProcessingStats stats = current_stats();
        if (config_.log_chunk_summary) {
            print_summary(std::cout, stats, "current cumulative stats after chunk " + chunk_label);
        }
        write_record_file(stats, "current cumulative stats after chunk " + chunk_label, "chunk-complete");
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
    out << "input_mode: ";
    if (config_.chunk_data_failure_action == ChunkDataFailureAction::DownloadAndParse) {
        out << (config_.persist_realtime_parsed_cache
                    ? "parsed-cache-with-persisted-chunk-recovery\n"
                    : "parsed-cache-with-realtime-chunk-recovery\n");
    } else if (!config_.parse_on_cache_miss) {
        out << "parsed-cache-only\n";
    } else if (config_.persist_realtime_parsed_cache) {
        out << "parsed-cache-with-persisted-on-demand-fallback\n";
    } else {
        out << "parsed-cache-with-realtime-mrt-fallback\n";
    }
    out << "parse_on_cache_miss: " << (config_.parse_on_cache_miss ? "true" : "false") << '\n';
    out << "persist_realtime_parsed_cache: "
        << (config_.persist_realtime_parsed_cache ? "true" : "false") << '\n';
    out << "chunk_data_failure_action: "
        << (config_.chunk_data_failure_action == ChunkDataFailureAction::DownloadAndParse
                ? "download_and_parse"
                : "stop")
        << '\n';
    out << "parsed_cache_schema_version: " << kParsedCacheSchemaVersion << '\n';
    out << "parsed_cache_order: timestamp-ascending-stable\n";
    out << "analysis_time_sorting: false\n";
    out << "automatic_downloads: "
        << (config_.chunk_data_failure_action == ChunkDataFailureAction::DownloadAndParse ? "true" : "false")
        << '\n';
    out << "original_file_eviction: disabled\n";
    out << "parser_workers: " << config_.parser_workers << '\n';
    out << "processor_concurrent_message_handling: "
        << (processor_uses_concurrent_message_handling_ ? "true" : "false") << '\n';
    out << "processor_strict_chronological_order: "
        << (processor_requires_strict_chronological_order_ ? "true" : "false") << '\n';
    out << "message_batch_size: " << config_.message_batch_size << '\n';
    out << "chunk_size: " << config_.chunk_size << '\n';
    out << "chunk_unit: " << chunk_unit_to_string(config_.chunk_unit) << '\n';
    out << "processed_chunks: " << stats.chunk_count << '\n';
    out << "files_used: " << stats.files_used << '\n';
    out << "visited_messages: " << stats.visited_messages << '\n';
    out << "rib_messages: " << stats.rib_messages << '\n';
    out << "announcement_messages: " << stats.announcement_messages << '\n';
    out << "withdrawal_messages: " << stats.withdrawal_messages << '\n';
    out << "peer_state_messages: " << stats.peer_state_messages << '\n';
    out << "end_of_rib_messages: " << stats.end_of_rib_messages << '\n';
    out << "realtime_parsed_files: " << stats.realtime_parsed_files << '\n';
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

void ChunkEngine::process_files(const std::vector<std::filesystem::path> &files,
                                const ClosedDateRange &chunk,
                                FileProgressDisplay &progress,
                                const SourceFileSet &force_realtime_parse_files) {
    if (files.empty()) {
        return;
    }

    // `files` preserves the resource list's initial-time ordering. A single
    // worker keeps that order across files when strict delivery is requested.
    const std::size_t worker_count = processor_requires_strict_chronological_order_
                                         ? 1
                                         : std::min<std::size_t>(files.size(),
                                                                 static_cast<std::size_t>(config_.parser_workers));

    std::atomic<std::size_t> next_file_index{0};
    std::mutex processor_mutex;
    std::mutex *const processor_mutex_ptr =
        processor_uses_concurrent_message_handling_ ? nullptr : &processor_mutex;
    std::mutex fatal_error_mutex;
    std::exception_ptr fatal_error;
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
                    const bool force_realtime_parse =
                        force_realtime_parse_files.find(file_path) != force_realtime_parse_files.end();
                    const FileTraversalResult file_result =
                        traverse_single_file(file_path, chunk, processor_mutex_ptr,
                                             force_realtime_parse);
                    record_processed_file(file_result);
                    progress.mark_batch_completed(
                        1, safe_file_size(force_realtime_parse ? file_path
                                                               : parsed_cache_path(file_path)));
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
}

ChunkEngine::FileTraversalResult ChunkEngine::traverse_single_file(const std::filesystem::path &file_path,
                                                                   const ClosedDateRange &chunk,
                                                                   std::mutex *processor_mutex,
                                                                   bool force_realtime_parse) {
    if (force_realtime_parse) {
        const MessageTraversalStats stats = traverse_mrt_file(
            config_, file_path, message_fields_,
            static_cast<std::size_t>(config_.message_batch_size), chunk,
            [&](std::vector<BGPMessage> &messages) {
                dispatch_message_batch(messages, processor_mutex);
            });
        return FileTraversalResult{stats, true};
    }
    const AnalysisInputTraversal traversal = traverse_analysis_input(
        config_, file_path, message_fields_, static_cast<std::size_t>(config_.message_batch_size), chunk,
        [&](std::vector<BGPMessage> &messages) { dispatch_message_batch(messages, processor_mutex); });
    return FileTraversalResult{traversal.stats, traversal.used_realtime_parser};
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
        const auto throw_order_error = [&](const MessageTimestamp &current,
                                           const MessageTimestamp &previous,
                                           const BGPMessage &message) {
            std::ostringstream error;
            error << "Parsed-cache chronological order violation: " << current.first << '.'
                  << std::setfill('0') << std::setw(6) << current.second;
            if (has_message_field(message_fields_, BGPMessageFields::SourceFile)) {
                error << " from " << message.source_file;
            }
            error << " follows " << previous.first << '.' << std::setfill('0') << std::setw(6)
                  << previous.second << "; regenerate parsed caches";
            throw std::runtime_error(error.str());
        };

        const MessageTimestamp first_timestamp = timestamp_of(messages.front());
        if (last_delivered_message_timestamp_.has_value() && first_timestamp < *last_delivered_message_timestamp_) {
            throw_order_error(first_timestamp, *last_delivered_message_timestamp_, messages.front());
        }
        MessageTimestamp previous_timestamp = first_timestamp;
        for (std::size_t index = 1; index < messages.size(); ++index) {
            const MessageTimestamp current_timestamp = timestamp_of(messages[index]);
            if (current_timestamp < previous_timestamp) {
                throw_order_error(current_timestamp, previous_timestamp, messages[index]);
            }
            previous_timestamp = current_timestamp;
        }
        delivered_timestamp = previous_timestamp;
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

void ChunkEngine::record_realtime_parsed_files(std::size_t file_count) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.realtime_parsed_files += file_count;
}

void ChunkEngine::record_processed_file(const FileTraversalResult &file_result) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    const MessageTraversalStats &file_stats = file_result.stats;
    stats_.files_used += 1;
    stats_.visited_messages += file_stats.visited_messages;
    stats_.rib_messages += file_stats.rib_messages;
    stats_.announcement_messages += file_stats.announcement_messages;
    stats_.withdrawal_messages += file_stats.withdrawal_messages;
    stats_.peer_state_messages += file_stats.peer_state_messages;
    stats_.end_of_rib_messages += file_stats.end_of_rib_messages;
    if (file_result.used_realtime_parser) {
        stats_.realtime_parsed_files += 1;
    }
}

}  // namespace bgpstream_runner
