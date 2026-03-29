#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "bgpstream_runner/message_processor.h"

namespace bgpstream_runner {

class PrefixAsStatsProcessor : public MessageProcessor {
   public:
    std::string_view name() const override;
    void handle_messages(const std::vector<BGPMessage> &messages) override;
    void print_summary(std::ostream &out) const override;

   private:
    std::uint64_t prefix_scoped_as_total() const;

    std::uint64_t usable_update_elements_ = 0;
    std::unordered_map<std::string, std::unordered_set<std::uint32_t>> prefix_to_ases_;
};

}  // namespace bgpstream_runner
