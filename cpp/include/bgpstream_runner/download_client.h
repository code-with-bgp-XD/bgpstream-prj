#pragma once

#include <filesystem>
#include <vector>

#include "bgpstream_runner/types.h"

namespace bgpstream_runner {

struct DownloadTarget {
    std::filesystem::path destination_path;
    std::filesystem::path local_path;
    std::uint64_t expected_size_bytes = 0;
};

class DownloadClient {
   public:
    explicit DownloadClient(Config config);

    std::vector<DownloadTarget> collect_targets(const ClosedDateRange &range, int limit_override) const;
    void download_range(const ClosedDateRange &range, int limit_override, bool show_progress = true) const;

   private:
    int resolve_limit(int limit_override) const;

    Config config_;
};

}  // namespace bgpstream_runner
