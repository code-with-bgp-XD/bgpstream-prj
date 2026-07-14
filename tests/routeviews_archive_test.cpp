#include <ctime>

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bgpstream_runner/common.h"
#include "routeviews_archive.h"

namespace {

using bgpstream_runner::ClosedDateRange;
using bgpstream_runner::routeviews_archive::UpdateEntry;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::time_t utc_epoch(int year, int month, int day, int hour = 0, int minute = 0) {
    std::tm utc{};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    return timegm(&utc);
}

void test_filename_parser() {
    using bgpstream_runner::routeviews_archive::parse_update_filename;

    const auto irregular = parse_update_filename("updates.20250123.0814.bz2");
    require(irregular.has_value(), "valid non-quarter-hour Route Views filename was rejected");
    require(irregular->filename == "updates.20250123.0814.bz2", "parsed filename changed unexpectedly");
    require(irregular->initial_time == utc_epoch(2025, 1, 23, 8, 14), "filename timestamp was parsed incorrectly");

    require(!parse_update_filename("updates.20250229.0000.bz2").has_value(),
            "invalid non-leap-year date was accepted");
    require(!parse_update_filename("updates.20250123.2415.bz2").has_value(), "invalid hour was accepted");
    require(!parse_update_filename("updates.20250123.0815.bz2.part").has_value(),
            "partial-download filename was accepted");
    require(!parse_update_filename("rib.20250123.0815.bz2").has_value(), "non-update filename was accepted");
}

void test_directory_index_parser() {
    using bgpstream_runner::routeviews_archive::parse_updates_index;

    const std::string html = R"HTML(
        <a href="../">Parent Directory</a>
        <a href="updates.20250123.0814.bz2">updates.20250123.081..&gt;</a>
        <a HREF = '/route-views.sg/bgpdata/2025.01/UPDATES/updates.20250123.0800.bz2?mirror=1'>file</a>
        <a href="updates.20250123.0814.bz2">duplicate</a>
        <a href="updates.20250123.0815.bz2.md5">checksum</a>
        <a href="updates.20250229.0000.bz2">invalid date</a>
    )HTML";

    const std::vector<UpdateEntry> entries = parse_updates_index(html);
    require(entries.size() == 2, "directory parser did not filter and deduplicate hrefs");
    require(entries[0].filename == "updates.20250123.0800.bz2", "directory entries were not time-sorted");
    require(entries[1].filename == "updates.20250123.0814.bz2", "irregular archive filename was not retained");
}

void test_month_enumeration() {
    using bgpstream_runner::routeviews_archive::months_for_range;

    const std::vector<std::string> one_day =
        months_for_range(ClosedDateRange{utc_epoch(2025, 1, 1), utc_epoch(2025, 1, 2)});
    require(one_day == std::vector<std::string>({"2024.12", "2025.01"}),
            "month-start range did not include the preceding boundary month");

    const std::vector<std::string> mid_month =
        months_for_range(ClosedDateRange{utc_epoch(2025, 6, 15), utc_epoch(2025, 6, 16)});
    require(mid_month == std::vector<std::string>({"2025.06"}), "mid-month range fetched unrelated months");

    const std::vector<std::string> full_year =
        months_for_range(ClosedDateRange{utc_epoch(2025, 1, 1), utc_epoch(2026, 1, 1)});
    require(full_year.size() == 14 && full_year.front() == "2024.12" && full_year.back() == "2026.01",
            "full-year range did not cover both boundary months");
}

void test_local_cache_month_path() {
    using bgpstream_runner::utc_year_month_path;

    require(utc_year_month_path(utc_epoch(2025, 1, 1)).generic_string() == "2025/01",
            "January cache path did not use YYYY/MM hierarchy");
    require(utc_year_month_path(utc_epoch(2024, 12, 31, 23, 45)).generic_string() == "2024/12",
            "cache path did not use the resource's UTC month");
}

void test_range_selection() {
    using bgpstream_runner::routeviews_archive::select_updates_for_range;

    std::vector<UpdateEntry> entries{
        {"before-a", 500},
        {"before-b", 500},
        {"start", 1000},
        {"inside", 1511},
        {"end-a", 2000},
        {"end-b", 2000},
        {"after", 2100},
        {"inside", 1511},
    };
    const std::vector<UpdateEntry> selected =
        select_updates_for_range(std::move(entries), ClosedDateRange{1000, 2000});
    require(selected.size() == 6, "range selection did not retain both boundary timestamp groups");
    require(selected.front().filename == "before-a" && selected.back().filename == "end-b",
            "range selection chose the wrong boundary resources");

    const std::vector<UpdateEntry> gap = select_updates_for_range(
        {{"predecessor", 500}, {"successor", 2500}}, ClosedDateRange{1000, 2000});
    require(gap.size() == 2 && gap[0].filename == "predecessor" && gap[1].filename == "successor",
            "empty interval did not retain its nearest surrounding resources");
}

}  // namespace

int main() {
    try {
        test_filename_parser();
        test_directory_index_parser();
        test_month_enumeration();
        test_local_cache_month_path();
        test_range_selection();
        std::cout << "Route Views archive discovery tests passed\n";
        return 0;
    } catch (const std::exception &exc) {
        std::cerr << exc.what() << '\n';
        return 1;
    }
}
