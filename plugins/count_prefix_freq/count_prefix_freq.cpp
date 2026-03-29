#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"

namespace {

struct CoverageRow {
    double top_ratio = 0.0;
    std::size_t prefix_count = 0;
    std::uint64_t covered_messages = 0;
    double share_of_all_messages_pct = 0.0;
};

std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);

    for (const char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }

    return escaped;
}

std::string csv_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);

    for (const char ch : value) {
        if (ch == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(ch);
        }
    }

    return escaped;
}

std::string shell_quote(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::string format_ratio_label(double top_ratio) {
    std::ostringstream output;
    output << "top_" << std::llround(top_ratio * 100.0) << "_pct";
    return output.str();
}

std::string format_timestamp_utc(std::chrono::system_clock::time_point now) {
    const std::time_t epoch = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm{};
    gmtime_r(&epoch, &utc_tm);

    std::ostringstream output;
    output << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

}  // namespace

class CountPrefixFreqProcessor : public bgpstream_runner::MessageProcessor {
   public:
    std::string_view name() const override { return "count_prefix_freq"; }

    void handle_messages(const std::vector<bgpstream_runner::BGPMessage> &messages) override {
        total_messages_seen_ += messages.size();

        for (const auto &message : messages) {
            if (message.prefix.empty()) {
                continue;
            }

            messages_with_prefix_ += 1;
            prefix_counts_[message.prefix] += 1;

            if (message.type == bgpstream_runner::BGPMessageType::Announcement) {
                announcement_messages_with_prefix_ += 1;
            } else if (message.type == bgpstream_runner::BGPMessageType::Withdrawal) {
                withdrawal_messages_with_prefix_ += 1;
            }
        }
    }

    void finalize() override {
        finalized_ = true;
        render_error_.clear();
        coverage_rows_.clear();

        const std::filesystem::path output_dir = ensure_output_dir();
        counts_csv_path_ = output_dir / "prefix_frequency_counts.csv";
        summary_json_path_ = output_dir / "prefix_frequency_summary.json";
        report_svg_path_ = output_dir / "prefix_frequency_report.svg";

        std::vector<std::pair<std::string, std::uint64_t>> sorted_counts(prefix_counts_.begin(), prefix_counts_.end());
        std::sort(sorted_counts.begin(), sorted_counts.end(), [](const auto &left, const auto &right) {
            if (left.second != right.second) {
                return left.second > right.second;
            }
            return left.first < right.first;
        });

        coverage_rows_ = build_coverage_rows(sorted_counts);
        write_counts_csv(sorted_counts);
        write_summary_json(sorted_counts);
        render_svg_report();
    }

    void print_summary(std::ostream &out) const override {
        out << "total_messages_seen: " << total_messages_seen_ << '\n';
        out << "messages_with_prefix: " << messages_with_prefix_ << '\n';
        out << "announcement_messages_with_prefix: " << announcement_messages_with_prefix_ << '\n';
        out << "withdrawal_messages_with_prefix: " << withdrawal_messages_with_prefix_ << '\n';
        out << "unique_prefixes: " << prefix_counts_.size() << '\n';

        if (!finalized_) {
            out << "prefix_frequency_report_status: pending_finalize" << '\n';
            return;
        }

        out << "prefix_frequency_counts_csv: " << counts_csv_path_.string() << '\n';
        out << "prefix_frequency_summary_json: " << summary_json_path_.string() << '\n';
        out << "prefix_frequency_report_svg: " << report_svg_path_.string() << '\n';

        for (const CoverageRow &row : coverage_rows_) {
            out << format_ratio_label(row.top_ratio) << "_prefixes: " << row.prefix_count << '\n';
            out << format_ratio_label(row.top_ratio) << "_covered_messages: " << row.covered_messages << '\n';
            out << format_ratio_label(row.top_ratio)
                << "_share_of_all_messages_pct: " << row.share_of_all_messages_pct << '\n';
        }

        if (!render_error_.empty()) {
            out << "prefix_frequency_render_status: failed" << '\n';
            out << "prefix_frequency_render_error: " << render_error_ << '\n';
        } else {
            out << "prefix_frequency_render_status: success" << '\n';
        }
    }

   private:
    std::filesystem::path source_dir() const { return std::filesystem::path(__FILE__).parent_path(); }

    std::filesystem::path ensure_output_dir() const {
        const std::filesystem::path output_dir = source_dir() / "output";
        std::error_code error;
        std::filesystem::create_directories(output_dir, error);
        if (error) {
            throw std::runtime_error("Failed to create output directory " + output_dir.string() + ": " +
                                     error.message());
        }
        return output_dir;
    }

    std::vector<CoverageRow> build_coverage_rows(
        const std::vector<std::pair<std::string, std::uint64_t>> &sorted_counts) const {
        static constexpr std::array<double, 5> kTopRatios = {0.01, 0.05, 0.10, 0.20, 0.50};

        std::vector<CoverageRow> rows;
        rows.reserve(kTopRatios.size());

        if (sorted_counts.empty()) {
            for (const double ratio : kTopRatios) {
                rows.push_back(CoverageRow{ratio, 0, 0, 0.0});
            }
            return rows;
        }

        std::vector<std::uint64_t> prefix_cumulative_counts(sorted_counts.size(), 0);
        std::uint64_t running_total = 0;
        for (std::size_t index = 0; index < sorted_counts.size(); ++index) {
            running_total += sorted_counts[index].second;
            prefix_cumulative_counts[index] = running_total;
        }

        for (const double ratio : kTopRatios) {
            const std::size_t prefix_count =
                std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(sorted_counts.size() * ratio)));
            const std::uint64_t covered_messages = prefix_cumulative_counts[prefix_count - 1];

            CoverageRow row;
            row.top_ratio = ratio;
            row.prefix_count = prefix_count;
            row.covered_messages = covered_messages;
            row.share_of_all_messages_pct =
                total_messages_seen_ == 0 ? 0.0 : 100.0 * static_cast<double>(covered_messages) /
                                                      static_cast<double>(total_messages_seen_);
            rows.push_back(row);
        }

        return rows;
    }

    void write_counts_csv(const std::vector<std::pair<std::string, std::uint64_t>> &sorted_counts) const {
        std::ofstream output(counts_csv_path_);
        if (!output) {
            throw std::runtime_error("Failed to open CSV output: " + counts_csv_path_.string());
        }

        output << "prefix,count\n";
        for (const auto &entry : sorted_counts) {
            output << '"' << csv_escape(entry.first) << '"' << ',' << entry.second << '\n';
        }
    }

    void write_summary_json(const std::vector<std::pair<std::string, std::uint64_t>> &sorted_counts) const {
        std::ofstream output(summary_json_path_);
        if (!output) {
            throw std::runtime_error("Failed to open JSON output: " + summary_json_path_.string());
        }

        const auto now = std::chrono::system_clock::now();
        output << "{\n";
        output << "  \"processor\": \"" << json_escape(std::string(name())) << "\",\n";
        output << "  \"generated_at_utc\": \"" << format_timestamp_utc(now) << "\",\n";
        output << "  \"total_messages_seen\": " << total_messages_seen_ << ",\n";
        output << "  \"messages_with_prefix\": " << messages_with_prefix_ << ",\n";
        output << "  \"announcement_messages_with_prefix\": " << announcement_messages_with_prefix_ << ",\n";
        output << "  \"withdrawal_messages_with_prefix\": " << withdrawal_messages_with_prefix_ << ",\n";
        output << "  \"unique_prefixes\": " << prefix_counts_.size() << ",\n";
        output << "  \"counts_csv\": \"" << json_escape(counts_csv_path_.string()) << "\",\n";
        output << "  \"coverage_rows\": [\n";
        for (std::size_t index = 0; index < coverage_rows_.size(); ++index) {
            const CoverageRow &row = coverage_rows_[index];
            output << "    {\n";
            output << "      \"label\": \"" << json_escape(format_ratio_label(row.top_ratio)) << "\",\n";
            output << "      \"top_ratio\": " << row.top_ratio << ",\n";
            output << "      \"prefix_count\": " << row.prefix_count << ",\n";
            output << "      \"covered_messages\": " << row.covered_messages << ",\n";
            output << "      \"share_of_all_messages_pct\": " << row.share_of_all_messages_pct << '\n';
            output << "    }";
            if (index + 1 != coverage_rows_.size()) {
                output << ',';
            }
            output << '\n';
        }
        output << "  ],\n";
        output << "  \"top_prefixes\": [\n";
        const std::size_t top_limit = std::min<std::size_t>(20, sorted_counts.size());
        for (std::size_t index = 0; index < top_limit; ++index) {
            output << "    {\n";
            output << "      \"prefix\": \"" << json_escape(sorted_counts[index].first) << "\",\n";
            output << "      \"count\": " << sorted_counts[index].second << '\n';
            output << "    }";
            if (index + 1 != top_limit) {
                output << ',';
            }
            output << '\n';
        }
        output << "  ]\n";
        output << "}\n";
    }

    void render_svg_report() {
        const std::filesystem::path script_path = source_dir() / "render_prefix_frequency_report.py";
        const std::string command = "python3 " + shell_quote(script_path.string()) + " --input " +
                                    shell_quote(summary_json_path_.string()) + " --output " +
                                    shell_quote(report_svg_path_.string());

        const int exit_code = std::system(command.c_str());
        if (exit_code != 0) {
            std::ostringstream error;
            error << "Python renderer exited with code " << exit_code;
            render_error_ = error.str();
            std::cerr << render_error_ << '\n';
        }
    }

    std::unordered_map<std::string, std::uint64_t> prefix_counts_;
    std::uint64_t total_messages_seen_ = 0;
    std::uint64_t messages_with_prefix_ = 0;
    std::uint64_t announcement_messages_with_prefix_ = 0;
    std::uint64_t withdrawal_messages_with_prefix_ = 0;
    bool finalized_ = false;
    std::vector<CoverageRow> coverage_rows_;
    std::filesystem::path counts_csv_path_;
    std::filesystem::path summary_json_path_;
    std::filesystem::path report_svg_path_;
    std::string render_error_;
};

BGPSTREAM_RUNNER_EXPORT_PROCESSOR(CountPrefixFreqProcessor)
