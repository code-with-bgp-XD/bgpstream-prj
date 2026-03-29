#pragma once

#include "bgpstream_runner/types.h"

#include <filesystem>
#include <vector>

namespace bgpstream_runner {

class DownloadClient {
public:
  explicit DownloadClient(Config config);

  std::vector<std::filesystem::path>
  collect_target_files(const ClosedDateRange &range, int limit_override) const;
  void download_range(const ClosedDateRange &range, int limit_override) const;

private:
  std::string build_download_command(const ClosedDateRange &range, bool dry_run,
                                     int limit_override) const;
  int resolve_limit(int limit_override) const;
  std::string download_script_path() const;
  std::string python_executable() const;

  Config config_;
};

} // namespace bgpstream_runner
