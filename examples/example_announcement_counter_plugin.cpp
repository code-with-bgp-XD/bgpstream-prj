#include <atomic>
#include <cstdint>

#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"

class ExampleAnnouncementCounterProcessor : public bgpstream_runner::MessageProcessor {
   public:
    std::string_view name() const override { return "example_announcement_counter"; }

    bool supports_concurrent_message_handling() const noexcept override { return true; }

    void handle_messages(const std::vector<bgpstream_runner::BGPMessage> &messages) override {
        std::uint64_t announcement_messages = 0;
        for (const auto &message : messages) {
            if (message.type == bgpstream_runner::BGPMessageType::Announcement) {
                announcement_messages += 1;
            }
        }
        announcement_messages_.fetch_add(announcement_messages, std::memory_order_relaxed);
    }

    void print_summary(std::ostream &out) const override {
        out << "announcement_messages_seen: " << announcement_messages_.load(std::memory_order_relaxed) << '\n';
    }

   private:
    std::atomic<std::uint64_t> announcement_messages_{0};
};

BGPSTREAM_RUNNER_EXPORT_PROCESSOR(ExampleAnnouncementCounterProcessor)
