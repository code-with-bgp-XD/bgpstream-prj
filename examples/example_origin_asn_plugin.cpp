#include <cstdint>
#include <unordered_set>

#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"

class ExampleOriginAsnProcessor : public bgpstream_runner::MessageProcessor {
   public:
    std::string_view name() const override { return "example_origin_asn"; }

    void handle_messages(const std::vector<bgpstream_runner::BGPMessage> &messages) override {
        for (const auto &message : messages) {
            if (message.asns.empty()) {
                continue;
            }

            origin_asns_.insert(message.asns.back());
            announcements_with_origin_ +=
                static_cast<std::uint64_t>(message.type == bgpstream_runner::BGPMessageType::Announcement);
        }
    }

    void print_summary(std::ostream &out) const override {
        out << "announcements_with_origin: " << announcements_with_origin_ << '\n';
        out << "unique_origin_asns: " << origin_asns_.size() << '\n';
    }

   private:
    std::uint64_t announcements_with_origin_ = 0;
    std::unordered_set<std::uint32_t> origin_asns_;
};

BGPSTREAM_RUNNER_EXPORT_PROCESSOR(ExampleOriginAsnProcessor)
