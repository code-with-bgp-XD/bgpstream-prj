#include <cstdint>
#include <string>
#include <unordered_set>

#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"

class ExampleMessageSummaryProcessor : public bgpstream_runner::MessageProcessor {
   public:
    std::string_view name() const override { return "example_message_summary"; }

    void handle_messages(const std::vector<bgpstream_runner::BGPMessage> &messages) override {
        processed_messages_ += messages.size();

        for (const auto &message : messages) {
            if (message.prefix.empty()) {
                continue;
            }

            unique_prefixes_.insert(message.prefix);
            if (message.type == bgpstream_runner::BGPMessageType::Announcement) {
                announcement_messages_with_prefix_ += 1;
            } else if (message.type == bgpstream_runner::BGPMessageType::Withdrawal) {
                withdrawal_messages_with_prefix_ += 1;
            }
        }
    }

    void print_summary(std::ostream &out) const override {
        out << "processed_messages: " << processed_messages_ << '\n';
        out << "announcement_messages_with_prefix: " << announcement_messages_with_prefix_ << '\n';
        out << "withdrawal_messages_with_prefix: " << withdrawal_messages_with_prefix_ << '\n';
        out << "unique_prefixes: " << unique_prefixes_.size() << '\n';
    }

   private:
    std::uint64_t processed_messages_ = 0;
    std::uint64_t announcement_messages_with_prefix_ = 0;
    std::uint64_t withdrawal_messages_with_prefix_ = 0;
    std::unordered_set<std::string> unique_prefixes_;
};

BGPSTREAM_RUNNER_EXPORT_PROCESSOR(ExampleMessageSummaryProcessor)
