extern "C" {
#include <bgpstream.h>
}

#include "bgpstream_runner/chunk_engine.h"

#include "bgpstream_runner/common.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>

namespace bgpstream_runner {

namespace {

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
    throw ParseFailure(std::string("Failed to stringify prefix: ") +
                       std::strerror(errno));
  }
  return std::string(buffer);
}

void append_asns_from_path(const bgpstream_as_path_t *as_path,
                           std::vector<std::uint32_t> *ases) {
  if (as_path == nullptr) {
    return;
  }

  bgpstream_as_path_iter_t iter;
  bgpstream_as_path_iter_reset(&iter);

  while (bgpstream_as_path_seg_t *seg =
             bgpstream_as_path_get_next_seg(as_path, &iter)) {
    switch (seg->type) {
    case BGPSTREAM_AS_PATH_SEG_ASN:
      ases->push_back(seg->asn.asn);
      break;
    case BGPSTREAM_AS_PATH_SEG_SET:
    case BGPSTREAM_AS_PATH_SEG_CONFED_SEQ:
    case BGPSTREAM_AS_PATH_SEG_CONFED_SET:
      for (std::uint8_t index = 0; index < seg->set.asn_cnt; ++index) {
        ases->push_back(seg->set.asn[index]);
      }
      break;
    default:
      break;
    }
  }
}

std::string format_parse_failure(const std::filesystem::path &file_path,
                                 const std::string &reason) {
  return "解析跳过: " + file_path.filename().string() + " | " + reason;
}

} // namespace

ChunkEngine::ChunkEngine(Config config, MessageProcessor &processor)
    : config_(std::move(config)), download_client_(config_),
      processor_(processor) {}

RangeProcessingStats ChunkEngine::run() {
  const ClosedDateRange range = parse_closed_date_range(config_);
  const std::vector<ClosedDateRange> chunks =
      split_range_by_chunks(range, config_.chunk_size, config_.chunk_unit);

  RangeProcessingStats stats;
  int remaining_limit = config_.limit;

  for (const ClosedDateRange &chunk : chunks) {
    if (remaining_limit == 0) {
      break;
    }

    stats.chunk_count += 1;
    const std::string chunk_label = format_range_label(chunk);
    std::vector<std::filesystem::path> target_files;

    try {
      if (config_.log_phase_transitions) {
        std::cout << "download phase " << chunk_label << std::endl;
      }
      target_files =
          download_client_.collect_target_files(chunk, remaining_limit);
      if (target_files.empty()) {
        if (config_.log_chunk_summary) {
          print_summary(std::cout, stats,
                        "current cumulative stats after chunk " + chunk_label);
        }
        continue;
      }

      download_client_.download_range(chunk, remaining_limit);

      std::vector<std::filesystem::path> existing_files;
      existing_files.reserve(target_files.size());
      for (const auto &file_path : target_files) {
        if (std::filesystem::exists(file_path)) {
          existing_files.push_back(file_path);
        }
      }

      if (!existing_files.empty()) {
        if (config_.log_phase_transitions) {
          std::cout << "process phase " << chunk_label << std::endl;
        }
        process_files(existing_files, chunk, &stats);
      } else {
        if (config_.log_phase_transitions) {
          std::cout << "skip process phase " << chunk_label
                    << " because no local files are available" << std::endl;
        }
      }

      if (config_.log_chunk_summary) {
        print_summary(std::cout, stats,
                      "current cumulative stats after chunk " + chunk_label);
      }
    } catch (...) {
      try {
        cleanup_chunk_files(target_files);
      } catch (const std::exception &cleanup_exc) {
        std::cerr << cleanup_exc.what() << '\n';
      }
      throw;
    }

    if (config_.log_phase_transitions) {
      std::cout << "cleanup phase " << chunk_label << std::endl;
    }
    cleanup_chunk_files(target_files);

    if (remaining_limit > 0) {
      remaining_limit -= static_cast<int>(target_files.size());
      if (remaining_limit < 0) {
        remaining_limit = 0;
      }
    }
  }

  return stats;
}

void ChunkEngine::print_summary(std::ostream &out,
                                const RangeProcessingStats &stats,
                                std::string_view title) const {
  const auto data_dir =
      config_.output_dir / config_.project / config_.collector / "updates";

  out << title << '\n';
  out << "processor: " << processor_.name() << '\n';
  out << "start_date: " << config_.start_date << '\n';
  out << "end_date: " << config_.end_date << '\n';
  out << "collector: " << config_.collector << '\n';
  out << "data_dir: " << std::filesystem::absolute(data_dir).string() << '\n';
  out << "download_workers: " << config_.download_workers << '\n';
  out << "parser_workers: " << config_.parser_workers << '\n';
  out << "message_batch_size: " << config_.message_batch_size << '\n';
  out << "chunk_size: " << config_.chunk_size << '\n';
  out << "chunk_unit: " << chunk_unit_to_string(config_.chunk_unit) << '\n';
  out << "processed_chunks: " << stats.chunk_count << '\n';
  out << "files_used: " << stats.files_used << '\n';
  out << "visited_messages: " << stats.visited_messages << '\n';
  out << "announcement_messages: " << stats.announcement_messages << '\n';
  out << "withdrawal_messages: " << stats.withdrawal_messages << '\n';
  out << "skipped_parse_files: " << stats.skipped_parse_files << '\n';
  processor_.print_summary(out);
  out << std::flush;
}

void ChunkEngine::process_files(const std::vector<std::filesystem::path> &files,
                                const ClosedDateRange &chunk,
                                RangeProcessingStats *stats) {
  if (files.empty()) {
    return;
  }

  FileProgressDisplay progress(files.size(), total_file_bytes(files));
  const std::size_t worker_count = std::min<std::size_t>(
      files.size(), static_cast<std::size_t>(config_.parser_workers));

  try {
    std::atomic<std::size_t> next_file_index{0};
    std::atomic<std::uint64_t> visited_messages{0};
    std::atomic<std::uint64_t> announcement_messages{0};
    std::atomic<std::uint64_t> withdrawal_messages{0};
    std::atomic<std::uint64_t> skipped_parse_files{0};
    std::mutex processor_mutex;
    std::mutex parse_failures_mutex;
    std::mutex fatal_error_mutex;
    std::exception_ptr fatal_error;
    std::vector<std::string> parse_failures;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker_index = 0; worker_index < worker_count;
         ++worker_index) {
      workers.emplace_back([&, worker_index]() {
        (void)worker_index;
        try {
          while (true) {
            const std::size_t file_index =
                next_file_index.fetch_add(1, std::memory_order_relaxed);
            if (file_index >= files.size()) {
              break;
            }

            const auto &file_path = files[file_index];
            try {
              const FileTraversalStats file_stats =
                  traverse_single_file(file_path, chunk, &processor_mutex);
              visited_messages.fetch_add(file_stats.visited_messages,
                                         std::memory_order_relaxed);
              announcement_messages.fetch_add(file_stats.announcement_messages,
                                              std::memory_order_relaxed);
              withdrawal_messages.fetch_add(file_stats.withdrawal_messages,
                                            std::memory_order_relaxed);
            } catch (const ParseFailure &exc) {
              skipped_parse_files.fetch_add(1, std::memory_order_relaxed);
              std::lock_guard<std::mutex> lock(parse_failures_mutex);
              parse_failures.push_back(
                  format_parse_failure(file_path, exc.what()));
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

    progress.finish();
    for (const auto &message : parse_failures) {
      std::cerr << message << '\n';
    }

    stats->files_used += files.size();
    stats->visited_messages += visited_messages.load(std::memory_order_relaxed);
    stats->announcement_messages +=
        announcement_messages.load(std::memory_order_relaxed);
    stats->withdrawal_messages +=
        withdrawal_messages.load(std::memory_order_relaxed);
    stats->skipped_parse_files +=
        skipped_parse_files.load(std::memory_order_relaxed);
  } catch (...) {
    progress.close();
    throw;
  }
}

ChunkEngine::FileTraversalStats
ChunkEngine::traverse_single_file(const std::filesystem::path &file_path,
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
    const auto interface_id =
        bgpstream_get_data_interface_id_by_name(stream, "singlefile");
    if (interface_id == _BGPSTREAM_DATA_INTERFACE_INVALID) {
      throw ParseFailure("singlefile data interface is unavailable");
    }

    bgpstream_set_data_interface(stream, interface_id);

    auto *upd_file_option = bgpstream_get_data_interface_option_by_name(
        stream, interface_id, "upd-file");
    if (upd_file_option == nullptr) {
      throw ParseFailure("singlefile/upd-file option is unavailable");
    }

    if (bgpstream_set_data_interface_option(stream, upd_file_option,
                                            file_path.string().c_str()) != 0) {
      throw ParseFailure("Failed to set upd-file option for " +
                         file_path.string());
    }

    if (bgpstream_start(stream) != 0) {
      throw ParseFailure("Failed to start BGPStream for " + file_path.string());
    }

    FileTraversalStats stats;
    std::vector<BGPMessage> message_batch;
    message_batch.reserve(static_cast<std::size_t>(config_.message_batch_size));

    auto flush_batch = [&]() {
      if (message_batch.empty()) {
        return;
      }
      std::lock_guard<std::mutex> lock(*processor_mutex);
      processor_.handle_messages(message_batch);
      message_batch.clear();
    };

    bgpstream_record_t *record = nullptr;
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

        BGPMessageType message_type;
        switch (elem->type) {
        case BGPSTREAM_ELEM_TYPE_ANNOUNCEMENT:
          message_type = BGPMessageType::Announcement;
          break;
        case BGPSTREAM_ELEM_TYPE_WITHDRAWAL:
          message_type = BGPMessageType::Withdrawal;
          break;
        default:
          continue;
        }

        const std::time_t timestamp =
            static_cast<std::time_t>(record->time_sec);
        if (!(chunk.start_epoch <= timestamp &&
              timestamp < chunk.end_exclusive_epoch)) {
          continue;
        }

        BGPMessage message;
        message.type = message_type;
        message.timestamp = timestamp;
        message.prefix = prefix_to_string(elem->prefix);
        if (message.type == BGPMessageType::Announcement) {
          append_asns_from_path(elem->as_path, &message.asns);
          stats.announcement_messages += 1;
        } else {
          stats.withdrawal_messages += 1;
        }
        stats.visited_messages += 1;

        message_batch.push_back(std::move(message));
        if (message_batch.size() >=
            static_cast<std::size_t>(config_.message_batch_size)) {
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

void ChunkEngine::cleanup_chunk_files(
    const std::vector<std::filesystem::path> &target_files) const {
  for (const auto &file_path : target_files) {
    std::error_code error;
    std::filesystem::remove(file_path, error);
    if (error) {
      throw std::runtime_error("Failed to remove chunk file " +
                               file_path.string() + ": " + error.message());
    }

    const auto partial_path =
        file_path.parent_path() / (file_path.filename().string() + ".part");
    error.clear();
    std::filesystem::remove(partial_path, error);
    if (error) {
      throw std::runtime_error("Failed to remove chunk partial file " +
                               partial_path.string() + ": " + error.message());
    }
  }
}

} // namespace bgpstream_runner
