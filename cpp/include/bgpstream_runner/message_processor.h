#pragma once

#include "bgpstream_runner/types.h"

#include <ostream>
#include <string_view>
#include <vector>

namespace bgpstream_runner {

class MessageProcessor {
 public:
  virtual ~MessageProcessor() = default;

  virtual std::string_view name() const = 0;
  virtual void handle_messages(const std::vector<BGPMessage> &messages) = 0;
  virtual void print_summary(std::ostream &out) const = 0;
};

}  // namespace bgpstream_runner
