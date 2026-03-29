#include <cstdint>

#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"

class ExampleAnnouncementCounterProcessor : public bgpstream_runner::MessageProcessor {
   public:
    std::string_view name() const override { return "example_announcement_counter"; }

    void handle_messages(const std::vector<bgpstream_runner::BGPMessage> &messages) override {
        for (const auto &message : messages) {
            if (message.type == bgpstream_runner::BGPMessageType::Announcement) {
                announcement_messages_ += 1;
            }
        }
    }

    void print_summary(std::ostream &out) const override {
        out << "announcement_messages_seen: " << announcement_messages_ << '\n';
    }

   private:
    std::uint64_t announcement_messages_ = 0;
};

BGPSTREAM_RUNNER_EXPORT_PROCESSOR(ExampleAnnouncementCounterProcessor)
