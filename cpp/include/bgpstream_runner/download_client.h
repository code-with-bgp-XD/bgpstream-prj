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
    void download_range(const ClosedDateRange &range, int limit_override) const;

   private:
    std::string build_download_command(const ClosedDateRange &range, bool dry_run, int limit_override,
                                       bool probe_size = false) const;
    int resolve_limit(int limit_override) const;
    std::string download_script_path() const;
    std::string python_executable() const;

    Config config_;
};

}  // namespace bgpstream_runner
