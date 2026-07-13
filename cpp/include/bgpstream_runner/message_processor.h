#pragma once

#include <ostream>
#include <string_view>
#include <vector>

#include "bgpstream_runner/types.h"

namespace bgpstream_runner {

class MessageProcessor {
   public:
    virtual ~MessageProcessor() = default;

    virtual std::string_view name() const = 0;
    virtual void handle_messages(const std::vector<BGPMessage> &messages) = 0;
    virtual void finalize() {}
    virtual void print_summary(std::ostream &out) const = 0;

    // Opt in only when handle_messages() can safely be called concurrently on
    // this processor instance. The engine never calls finalize() or
    // print_summary() while a handle_messages() call is still running.
    virtual bool supports_concurrent_message_handling() const noexcept { return false; }

    // Request nondecreasing (timestamp, timestamp_microseconds) order across
    // every handle_messages() call. This takes precedence over concurrent
    // message handling.
    virtual bool requires_strict_chronological_order() const noexcept { return false; }
};

}  // namespace bgpstream_runner
