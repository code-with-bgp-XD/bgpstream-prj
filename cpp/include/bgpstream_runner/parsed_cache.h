#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "bgpstream_runner/mrt_parser.h"

namespace bgpstream_runner {

inline constexpr std::uint32_t kParsedCacheSchemaVersion = 1;

class ParsedCacheFailure : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

enum class ParsedCacheState {
    Valid,
    Missing,
    Invalid,
};

struct ParsedCacheInspection {
    ParsedCacheState state = ParsedCacheState::Missing;
    std::filesystem::path cache_path;
    std::string reason;
};

struct ParsedCacheBuildSummary {
    std::size_t source_files = 0;
    std::size_t reused_files = 0;
    std::size_t generated_files = 0;
    std::uint64_t generated_messages = 0;
    std::uint64_t source_bytes = 0;
    std::uint64_t cache_bytes = 0;
};

std::filesystem::path parsed_cache_path(const std::filesystem::path &source_file);
ParsedCacheInspection inspect_parsed_cache(const std::filesystem::path &source_file);

class ParsedCacheWriter {
   public:
    explicit ParsedCacheWriter(std::filesystem::path source_file);
    ~ParsedCacheWriter();

    ParsedCacheWriter(const ParsedCacheWriter &) = delete;
    ParsedCacheWriter &operator=(const ParsedCacheWriter &) = delete;

    void append(const std::vector<BGPMessage> &messages);
    void finalize();
    const MessageTraversalStats &stats() const noexcept;
    const std::filesystem::path &output_path() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

MessageTraversalStats generate_parsed_cache(const Config &config, const std::filesystem::path &source_file,
                                            std::size_t message_batch_size);

MessageTraversalStats read_parsed_cache(const std::filesystem::path &source_file, BGPMessageFields fields,
                                        std::size_t message_batch_size,
                                        const std::optional<ClosedDateRange> &range,
                                        const MessageBatchHandler &handle_batch);

ParsedCacheBuildSummary ensure_parsed_caches(const Config &config,
                                             const std::vector<std::filesystem::path> &source_files,
                                             bool show_progress = true);

}  // namespace bgpstream_runner
