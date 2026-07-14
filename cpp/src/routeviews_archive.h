#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bgpstream_runner/types.h"

namespace bgpstream_runner::routeviews_archive {

inline constexpr std::time_t kUpdateDurationSeconds = 15 * 60;

struct UpdateEntry {
    std::string filename;
    std::time_t initial_time = 0;
};

std::optional<UpdateEntry> parse_update_filename(std::string_view filename);
std::vector<UpdateEntry> parse_updates_index(std::string_view html);
std::vector<std::string> months_for_range(const ClosedDateRange &range);
std::vector<UpdateEntry> select_updates_for_range(std::vector<UpdateEntry> entries,
                                                  const ClosedDateRange &range);

}  // namespace bgpstream_runner::routeviews_archive
