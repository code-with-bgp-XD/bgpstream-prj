#include "bgpstream_runner/prefix_as_stats_processor.h"

namespace bgpstream_runner {

std::string_view PrefixAsStatsProcessor::name() const {
  return "prefix_as_stats";
}

// 这个函数同一时间只会有一个线程在调用.
void PrefixAsStatsProcessor::handle_messages(const std::vector<BGPMessage> &messages) {
  for (const auto &message : messages) {
    if (message.type != BGPMessageType::Announcement) {
      continue;
    }
    if (message.prefix.empty() || message.asns.empty()) {
      continue;
    }

    usable_update_elements_ += 1;
    auto &ases = prefix_to_ases_[message.prefix];
    ases.insert(message.asns.begin(), message.asns.end());
  }
}

void PrefixAsStatsProcessor::print_summary(std::ostream &out) const {
  out << "usable_update_elements: " << usable_update_elements_ << '\n';
  out << "unique_prefixes: " << prefix_to_ases_.size() << '\n';
  out << "prefix_scoped_as_total: " << prefix_scoped_as_total() << '\n';
}

std::uint64_t PrefixAsStatsProcessor::prefix_scoped_as_total() const {
  std::uint64_t total = 0;
  for (const auto &entry : prefix_to_ases_) {
    total += entry.second.size();
  }
  return total;
}

}  // namespace bgpstream_runner
