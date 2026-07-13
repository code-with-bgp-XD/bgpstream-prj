#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"

namespace {

#ifndef BGPSTREAM_TIMESTAMP_LOG_DIR
#define BGPSTREAM_TIMESTAMP_LOG_DIR "log"
#endif

using MessageTimestamp = std::pair<std::time_t, std::uint32_t>;

std::string format_timestamp(const MessageTimestamp &timestamp) {
    std::tm utc_time{};
    std::ostringstream output;

    if (gmtime_r(&timestamp.first, &utc_time) != nullptr) {
        output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(6)
               << timestamp.second << 'Z';
    } else {
        output << timestamp.first << '.' << std::setfill('0') << std::setw(6) << timestamp.second;
    }

    return output.str();
}

std::filesystem::path make_timestamp_log_path() {
    const std::filesystem::path log_directory = BGPSTREAM_TIMESTAMP_LOG_DIR;
    std::error_code error;
    std::filesystem::create_directories(log_directory, error);
    if (error) {
        throw std::runtime_error("Failed to create timestamp log directory " + log_directory.string() + ": " +
                                 error.message());
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    if (localtime_r(&now_time, &local_time) == nullptr) {
        throw std::runtime_error("Failed to format timestamp log file name");
    }

    std::ostringstream base_name;
    base_name << "message-timestamps-" << std::put_time(&local_time, "%Y%m%d-%H%M%S");

    for (std::uint32_t suffix = 0;; ++suffix) {
        std::ostringstream file_name;
        file_name << base_name.str();
        if (suffix != 0) {
            file_name << '-' << std::setfill('0') << std::setw(3) << suffix;
        }
        file_name << ".log";

        const std::filesystem::path candidate = log_directory / file_name.str();
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
}

}  // namespace

class TimestampOrderCheckerProcessor : public bgpstream_runner::MessageProcessor {
   public:
    TimestampOrderCheckerProcessor()
        : timestamp_log_path_(make_timestamp_log_path()), timestamp_log_(timestamp_log_path_) {
        if (!timestamp_log_) {
            throw std::runtime_error("Failed to open timestamp log file: " + timestamp_log_path_.string());
        }
    }

    std::string_view name() const override { return "timestamp_order_checker"; }

    bool requires_strict_chronological_order() const noexcept override { return true; }

    void handle_messages(const std::vector<bgpstream_runner::BGPMessage> &messages) override {
        for (const auto &message : messages) {
            const MessageTimestamp current_timestamp{message.timestamp, message.timestamp_microseconds};
            const char *order = "first";

            if (previous_timestamp_.has_value()) {
                if (current_timestamp < *previous_timestamp_) {
                    order = "BACKWARD";
                    ++backward_transitions_;
                    if (!first_backward_transition_.has_value()) {
                        first_backward_transition_ = std::make_pair(*previous_timestamp_, current_timestamp);
                    }
                } else if (current_timestamp == *previous_timestamp_) {
                    order = "equal";
                    ++equal_timestamp_transitions_;
                } else {
                    order = "forward";
                }
            } else {
                first_timestamp_ = current_timestamp;
            }

            timestamp_log_ << "message[" << processed_messages_
                           << "] timestamp=" << format_timestamp(current_timestamp)
                           << " epoch=" << current_timestamp.first << '.' << std::setfill('0') << std::setw(6)
                           << current_timestamp.second << std::setfill(' ') << " order=" << order << '\n';

            previous_timestamp_ = current_timestamp;
            last_timestamp_ = current_timestamp;
            ++processed_messages_;
        }

        timestamp_log_.flush();
        if (!timestamp_log_) {
            throw std::runtime_error("Failed to write timestamp log file: " + timestamp_log_path_.string());
        }
    }

    void finalize() override {
        timestamp_log_.flush();
        if (!timestamp_log_) {
            throw std::runtime_error("Failed to finalize timestamp log file: " + timestamp_log_path_.string());
        }
    }

    void print_summary(std::ostream &out) const override {
        out << "timestamp_log_file: " << timestamp_log_path_.string() << '\n';
        out << "processed_messages: " << processed_messages_ << '\n';
        out << "backward_transitions: " << backward_transitions_ << '\n';
        out << "equal_timestamp_transitions: " << equal_timestamp_transitions_ << '\n';
        out << "timestamps_nondecreasing: " << (backward_transitions_ == 0 ? "true" : "false") << '\n';
        out << "timestamps_strictly_increasing: "
            << (backward_transitions_ == 0 && equal_timestamp_transitions_ == 0 ? "true" : "false") << '\n';

        if (first_timestamp_.has_value()) {
            out << "first_timestamp: " << format_timestamp(*first_timestamp_) << '\n';
            out << "last_timestamp: " << format_timestamp(*last_timestamp_) << '\n';
        } else {
            out << "first_timestamp: n/a\n";
            out << "last_timestamp: n/a\n";
        }

        if (first_backward_transition_.has_value()) {
            out << "first_backward_previous_timestamp: "
                << format_timestamp(first_backward_transition_->first) << '\n';
            out << "first_backward_current_timestamp: "
                << format_timestamp(first_backward_transition_->second) << '\n';
        }
    }

   private:
    std::filesystem::path timestamp_log_path_;
    std::ofstream timestamp_log_;
    std::uint64_t processed_messages_ = 0;
    std::uint64_t backward_transitions_ = 0;
    std::uint64_t equal_timestamp_transitions_ = 0;
    std::optional<MessageTimestamp> previous_timestamp_;
    std::optional<MessageTimestamp> first_timestamp_;
    std::optional<MessageTimestamp> last_timestamp_;
    std::optional<std::pair<MessageTimestamp, MessageTimestamp>> first_backward_transition_;
};

BGPSTREAM_RUNNER_EXPORT_PROCESSOR(TimestampOrderCheckerProcessor)
