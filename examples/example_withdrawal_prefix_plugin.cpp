#include <cstdint>
#include <string>
#include <unordered_set>

#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"

class ExampleWithdrawalPrefixProcessor : public bgpstream_runner::MessageProcessor {
   public:
    std::string_view name() const override { return "example_withdrawal_prefix"; }

    void handle_messages(const std::vector<bgpstream_runner::BGPMessage> &messages) override {
        for (const auto &message : messages) {
            if (message.type != bgpstream_runner::BGPMessageType::Withdrawal || message.prefix.empty()) {
                continue;
            }

            withdrawal_messages_with_prefix_ += 1;
            unique_withdrawn_prefixes_.insert(message.prefix);
        }
    }

    void print_summary(std::ostream &out) const override {
        out << "withdrawal_messages_with_prefix: " << withdrawal_messages_with_prefix_ << '\n';
        out << "unique_withdrawn_prefixes: " << unique_withdrawn_prefixes_.size() << '\n';
    }

   private:
    std::uint64_t withdrawal_messages_with_prefix_ = 0;
    std::unordered_set<std::string> unique_withdrawn_prefixes_;
};

BGPSTREAM_RUNNER_EXPORT_PROCESSOR(ExampleWithdrawalPrefixProcessor)
