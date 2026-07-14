#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

#include "bgpstream_runner/types.h"

namespace bgpstream_runner {

class MrtParseFailure : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

struct MessageTraversalStats {
    std::uint64_t visited_messages = 0;
    std::uint64_t rib_messages = 0;
    std::uint64_t announcement_messages = 0;
    std::uint64_t withdrawal_messages = 0;
    std::uint64_t peer_state_messages = 0;
    std::uint64_t end_of_rib_messages = 0;
};

using MessageBatchHandler = std::function<void(std::vector<BGPMessage> &)>;

// Parses one complete MRT file. `range` only controls which decoded messages
// are delivered; omitting it is required when building a reusable cache.
MessageTraversalStats traverse_mrt_file(const Config &config, const std::filesystem::path &file_path,
                                        BGPMessageFields fields, std::size_t message_batch_size,
                                        const std::optional<ClosedDateRange> &range,
                                        const MessageBatchHandler &handle_batch);

}  // namespace bgpstream_runner
