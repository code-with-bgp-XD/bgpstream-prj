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
        std::vector<DownloadTarget> targets;
        std::vector<std::filesystem::path> source_files;
    };

    if (config_.log_phase_transitions) {
        std::cout << "plan phase " << format_range_label(range) << std::endl;
    }

    std::vector<PlannedChunk> planned_chunks;
    planned_chunks.reserve(chunks.size());
    std::size_t total_files = 0;
    std::uint64_t total_bytes = 0;
    int remaining_limit = config_.limit;
    std::vector<std::string> unavailable_files;

    for (const ClosedDateRange &chunk : chunks) {
        if (remaining_limit == 0) {
            break;
        }

        PlannedChunk planned_chunk;
        planned_chunk.range = chunk;
        planned_chunk.label = format_range_label(chunk);
        planned_chunk.targets = download_client_.collect_targets(chunk, remaining_limit);
        planned_chunk.source_files.reserve(planned_chunk.targets.size());

        // Boundary resources can be traversed once per adjacent chunk because each
        // traversal applies a different time filter, so count every planned target.
        total_files += planned_chunk.targets.size();
        for (const DownloadTarget &target : planned_chunk.targets) {
            std::filesystem::path source_file;
            if (std::filesystem::exists(target.local_path)) {
                source_file = target.local_path;
            } else if (std::filesystem::exists(target.destination_path)) {
                source_file = target.destination_path;
            } else {
                unavailable_files.push_back("source MRT missing: " + target.destination_path.string());
                continue;
            }

            const ParsedCacheInspection inspection = inspect_parsed_cache(source_file);
            if (inspection.state != ParsedCacheState::Valid) {
                unavailable_files.push_back(inspection.cache_path.string() + " | " + inspection.reason);
                continue;
            }
            planned_chunk.source_files.push_back(std::move(source_file));
            total_bytes += safe_file_size(inspection.cache_path);
        }

        if (remaining_limit > 0) {
            remaining_limit -= static_cast<int>(planned_chunk.targets.size());
            if (remaining_limit < 0) {
                remaining_limit = 0;
            }
        }
        planned_chunks.push_back(std::move(planned_chunk));
    }

    if (!unavailable_files.empty()) {
        std::ostringstream message;
        message << "Analysis requires complete source MRT files and parsed caches; no files were downloaded or "
                   "generated. Missing or invalid entries: "
                << unavailable_files.size();
        const std::size_t displayed_count = std::min<std::size_t>(unavailable_files.size(), 8);
        for (std::size_t index = 0; index < displayed_count; ++index) {
            message << "\n  " << unavailable_files[index];
        }
        if (unavailable_files.size() > displayed_count) {
            message << "\n  ... and " << unavailable_files.size() - displayed_count << " more";
        }
        message << "\nRun ./manage.sh download (or download-release) to download missing MRT files and "
                   "generate parsed caches.";
        throw std::runtime_error(message.str());
    }

    std::unique_ptr<FileProgressDisplay> progress;
    if (total_files > 0) {
        progress = std::make_unique<FileProgressDisplay>(total_files, total_bytes, "parsed-cache");
    }

    for (const PlannedChunk &planned_chunk : planned_chunks) {
        const ClosedDateRange &chunk = planned_chunk.range;
        const std::string &chunk_label = planned_chunk.label;
        const std::vector<std::filesystem::path> &source_files = planned_chunk.source_files;

        increment_chunk_count();

        if (source_files.empty()) {
            const RangeProcessingStats stats = current_stats();
            if (config_.log_chunk_summary) {
                print_summary(std::cout, stats, "current cumulative stats after chunk " + chunk_label);
            }
            write_record_file(stats, "current cumulative stats after chunk " + chunk_label, "chunk-complete");
            continue;
        }

        if (config_.log_phase_transitions) {
            std::cout << "process parsed-cache phase " << chunk_label << std::endl;
        }
        process_files(source_files, chunk, *progress);

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
    out << "input_mode: parsed-cache-only\n";
    out << "parsed_cache_schema_version: " << kParsedCacheSchemaVersion << '\n';
    out << "parsed_cache_order: timestamp-ascending-stable\n";
    out << "analysis_time_sorting: false\n";
    out << "automatic_downloads: false\n";
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
                    const MessageTraversalStats file_stats =
                        traverse_single_file(file_path, chunk, processor_mutex_ptr);
                    record_processed_file(file_stats);
                    progress.mark_batch_completed(1, safe_file_size(parsed_cache_path(file_path)));
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

MessageTraversalStats ChunkEngine::traverse_single_file(const std::filesystem::path &file_path,
                                                        const ClosedDateRange &chunk,
                                                        std::mutex *processor_mutex) {
    return read_parsed_cache(file_path, message_fields_, static_cast<std::size_t>(config_.message_batch_size),
                             chunk, [&](std::vector<BGPMessage> &messages) {
                                 dispatch_message_batch(messages, processor_mutex);
                             });
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

void ChunkEngine::record_processed_file(const MessageTraversalStats &file_stats) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.files_used += 1;
    stats_.visited_messages += file_stats.visited_messages;
    stats_.rib_messages += file_stats.rib_messages;
    stats_.announcement_messages += file_stats.announcement_messages;
    stats_.withdrawal_messages += file_stats.withdrawal_messages;
    stats_.peer_state_messages += file_stats.peer_state_messages;
    stats_.end_of_rib_messages += file_stats.end_of_rib_messages;
}

}  // namespace bgpstream_runner
