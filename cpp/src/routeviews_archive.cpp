#include "routeviews_archive.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace bgpstream_runner::routeviews_archive {

namespace {

constexpr std::string_view kFilenamePrefix = "updates.";
constexpr std::string_view kFilenameSuffix = ".bz2";
constexpr std::size_t kTimestampLength = 13;
constexpr std::size_t kFilenameLength = kFilenamePrefix.size() + kTimestampLength + kFilenameSuffix.size();

bool ascii_equal_case_insensitive(char lhs, char rhs) {
    return std::tolower(static_cast<unsigned char>(lhs)) ==
           std::tolower(static_cast<unsigned char>(rhs));
}

bool starts_with_case_insensitive(std::string_view text, std::size_t position, std::string_view expected) {
    if (position > text.size() || expected.size() > text.size() - position) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (!ascii_equal_case_insensitive(text[position + index], expected[index])) {
            return false;
        }
    }
    return true;
}

std::optional<int> parse_digits(std::string_view text) {
    int value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<int>(ch - '0');
    }
    return value;
}

bool same_utc_fields(std::time_t epoch, int year, int month, int day, int hour, int minute) {
    std::tm round_trip{};
    return gmtime_r(&epoch, &round_trip) != nullptr && round_trip.tm_year == year - 1900 &&
           round_trip.tm_mon == month - 1 && round_trip.tm_mday == day && round_trip.tm_hour == hour &&
           round_trip.tm_min == minute && round_trip.tm_sec == 0;
}

std::string_view filename_from_href(std::string_view href) {
    const std::size_t query = href.find_first_of("?#");
    href = href.substr(0, query);
    const std::size_t slash = href.find_last_of('/');
    if (slash != std::string_view::npos) {
        href.remove_prefix(slash + 1);
    }
    return href;
}

std::vector<std::string_view> extract_hrefs(std::string_view html) {
    std::vector<std::string_view> hrefs;
    for (std::size_t position = 0; position < html.size();) {
        if (!starts_with_case_insensitive(html, position, "href")) {
            ++position;
            continue;
        }

        const bool valid_left_boundary =
            position == 0 || std::isspace(static_cast<unsigned char>(html[position - 1])) != 0 ||
            html[position - 1] == '<';
        if (!valid_left_boundary) {
            position += 4;
            continue;
        }

        std::size_t cursor = position + 4;
        while (cursor < html.size() && std::isspace(static_cast<unsigned char>(html[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= html.size() || html[cursor] != '=') {
            position += 4;
            continue;
        }
        ++cursor;
        while (cursor < html.size() && std::isspace(static_cast<unsigned char>(html[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= html.size() || (html[cursor] != '"' && html[cursor] != '\'')) {
            position += 4;
            continue;
        }

        const char quote = html[cursor++];
        const std::size_t end = html.find(quote, cursor);
        if (end == std::string_view::npos) {
            break;
        }
        hrefs.push_back(html.substr(cursor, end - cursor));
        position = end + 1;
    }
    return hrefs;
}

std::pair<int, int> utc_year_month(std::time_t epoch) {
    std::tm utc{};
    if (gmtime_r(&epoch, &utc) == nullptr) {
        throw std::runtime_error("Failed to convert Route Views archive month to UTC");
    }
    return {utc.tm_year + 1900, utc.tm_mon + 1};
}

std::string format_month(int year, int month) {
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << year << '.' << std::setw(2) << month;
    return output.str();
}

void advance_month(int *year, int *month) {
    if (*month == 12) {
        ++*year;
        *month = 1;
    } else {
        ++*month;
    }
}

}  // namespace

std::optional<UpdateEntry> parse_update_filename(std::string_view filename) {
    if (filename.size() != kFilenameLength || filename.substr(0, kFilenamePrefix.size()) != kFilenamePrefix ||
        filename.substr(filename.size() - kFilenameSuffix.size()) != kFilenameSuffix) {
        return std::nullopt;
    }

    const std::string_view timestamp =
        filename.substr(kFilenamePrefix.size(), kTimestampLength);
    if (timestamp[8] != '.') {
        return std::nullopt;
    }

    const std::optional<int> year = parse_digits(timestamp.substr(0, 4));
    const std::optional<int> month = parse_digits(timestamp.substr(4, 2));
    const std::optional<int> day = parse_digits(timestamp.substr(6, 2));
    const std::optional<int> hour = parse_digits(timestamp.substr(9, 2));
    const std::optional<int> minute = parse_digits(timestamp.substr(11, 2));
    if (!year.has_value() || !month.has_value() || !day.has_value() || !hour.has_value() ||
        !minute.has_value() || *month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour < 0 ||
        *hour > 23 || *minute < 0 || *minute > 59) {
        return std::nullopt;
    }

    std::tm utc{};
    utc.tm_year = *year - 1900;
    utc.tm_mon = *month - 1;
    utc.tm_mday = *day;
    utc.tm_hour = *hour;
    utc.tm_min = *minute;
    const std::time_t epoch = timegm(&utc);
    if (!same_utc_fields(epoch, *year, *month, *day, *hour, *minute)) {
        return std::nullopt;
    }

    return UpdateEntry{std::string(filename), epoch};
}

std::vector<UpdateEntry> parse_updates_index(std::string_view html) {
    std::vector<UpdateEntry> entries;
    for (const std::string_view href : extract_hrefs(html)) {
        const std::optional<UpdateEntry> entry = parse_update_filename(filename_from_href(href));
        if (entry.has_value()) {
            entries.push_back(*entry);
        }
    }

    std::sort(entries.begin(), entries.end(), [](const UpdateEntry &lhs, const UpdateEntry &rhs) {
        return std::tie(lhs.initial_time, lhs.filename) < std::tie(rhs.initial_time, rhs.filename);
    });
    entries.erase(std::unique(entries.begin(), entries.end(), [](const UpdateEntry &lhs, const UpdateEntry &rhs) {
                      return lhs.filename == rhs.filename;
                  }),
                  entries.end());
    return entries;
}

std::vector<std::string> months_for_range(const ClosedDateRange &range) {
    if (range.end_exclusive_epoch <= range.start_epoch) {
        throw std::runtime_error("Route Views resource range must not be empty");
    }

    const std::time_t first_lookup = range.start_epoch - kUpdateDurationSeconds;
    auto [year, month] = utc_year_month(first_lookup);
    const auto [last_year, last_month] = utc_year_month(range.end_exclusive_epoch);

    std::vector<std::string> months;
    while (year < last_year || (year == last_year && month <= last_month)) {
        months.push_back(format_month(year, month));
        advance_month(&year, &month);
    }
    return months;
}

std::vector<UpdateEntry> select_updates_for_range(std::vector<UpdateEntry> entries,
                                                  const ClosedDateRange &range) {
    if (range.end_exclusive_epoch <= range.start_epoch) {
        throw std::runtime_error("Route Views resource range must not be empty");
    }
    if (entries.empty()) {
        return {};
    }

    std::sort(entries.begin(), entries.end(), [](const UpdateEntry &lhs, const UpdateEntry &rhs) {
        return std::tie(lhs.initial_time, lhs.filename) < std::tie(rhs.initial_time, rhs.filename);
    });
    entries.erase(std::unique(entries.begin(), entries.end(), [](const UpdateEntry &lhs, const UpdateEntry &rhs) {
                      return lhs.filename == rhs.filename;
                  }),
                  entries.end());

    auto first = std::lower_bound(entries.begin(), entries.end(), range.start_epoch,
                                  [](const UpdateEntry &entry, std::time_t epoch) {
                                      return entry.initial_time < epoch;
                                  });
    if (first != entries.begin()) {
        const std::time_t predecessor_time = std::prev(first)->initial_time;
        first = std::lower_bound(entries.begin(), first, predecessor_time,
                                 [](const UpdateEntry &entry, std::time_t epoch) {
                                     return entry.initial_time < epoch;
                                 });
    }

    auto last = std::lower_bound(entries.begin(), entries.end(), range.end_exclusive_epoch,
                                 [](const UpdateEntry &entry, std::time_t epoch) {
                                     return entry.initial_time < epoch;
                                 });
    if (last != entries.end()) {
        const std::time_t successor_time = last->initial_time;
        last = std::upper_bound(last, entries.end(), successor_time,
                                [](std::time_t epoch, const UpdateEntry &entry) {
                                    return epoch < entry.initial_time;
                                });
    }

    if (first >= last) {
        return {};
    }
    return std::vector<UpdateEntry>(first, last);
}

}  // namespace bgpstream_runner::routeviews_archive
