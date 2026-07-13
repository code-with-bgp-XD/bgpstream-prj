#include <cstdint>
#include <unordered_set>

#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"

class ExampleOriginAsnProcessor : public bgpstream_runner::MessageProcessor {
   public:
    std::string_view name() const override { return "example_origin_asn"; }

    bgpstream_runner::BGPMessageFields required_message_fields() const noexcept override {
        return bgpstream_runner::BGPMessageFields::Type | bgpstream_runner::BGPMessageFields::OriginAsn;
    }

    void handle_messages(const std::vector<bgpstream_runner::BGPMessage> &messages) override {
        for (const auto &message : messages) {
            if (message.type != bgpstream_runner::BGPMessageType::Announcement || !message.origin_asn.has_value()) {
                continue;
            }

            origin_asns_.insert(*message.origin_asn);
            announcements_with_origin_ += 1;
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
