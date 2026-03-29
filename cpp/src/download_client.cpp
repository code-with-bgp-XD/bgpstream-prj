#include "bgpstream_runner/download_client.h"

#include "bgpstream_runner/common.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace bgpstream_runner {

namespace {

#ifndef BGPSTREAM_DOWNLOAD_SCRIPT_PATH
#define BGPSTREAM_DOWNLOAD_SCRIPT_PATH "python/download.py"
#endif

#ifndef BGPSTREAM_SOURCE_DIR
#define BGPSTREAM_SOURCE_DIR "."
#endif

} // namespace

DownloadClient::DownloadClient(Config config) : config_(std::move(config)) {}

std::vector<std::filesystem::path>
DownloadClient::collect_target_files(const ClosedDateRange &range,
                                     int limit_override) const {
  const CommandResult result =
      run_capture_command(build_download_command(range, true, limit_override));
  if (result.exit_code != 0) {
    throw std::runtime_error("download.py --dry-run failed with exit code " +
                             std::to_string(result.exit_code) + "\n" +
                             result.output);
  }

  std::vector<std::filesystem::path> files;
  std::istringstream lines(result.output);
  std::string line;
  while (std::getline(lines, line)) {
    const auto arrow = line.rfind(" -> ");
    if (arrow == std::string::npos) {
      continue;
    }
    const std::string path_text = trim(line.substr(arrow + 4));
    if (!path_text.empty()) {
      files.emplace_back(path_text);
    }
  }
  return files;
}

void DownloadClient::download_range(const ClosedDateRange &range,
                                    int limit_override) const {
  const int exit_code = run_streaming_command(
      build_download_command(range, false, limit_override));
  if (exit_code != 0) {
    throw std::runtime_error("download.py failed with exit code " +
                             std::to_string(exit_code));
  }
}

std::string DownloadClient::build_download_command(const ClosedDateRange &range,
                                                   bool dry_run,
                                                   int limit_override) const {
  std::ostringstream command;
  command << shell_escape(python_executable()) << " "
          << shell_escape(download_script_path()) << " --from-time "
          << shell_escape(format_utc_timestamp(range.start_epoch))
          << " --until-time "
          << shell_escape(format_utc_timestamp(range.end_exclusive_epoch))
          << " --collector " << shell_escape(config_.collector) << " --project "
          << shell_escape(config_.project) << " --record-type updates"
          << " --source auto"
          << " --workers " << config_.download_workers << " --output-dir "
          << shell_escape(config_.output_dir.string());

  const int limit = resolve_limit(limit_override);
  if (limit > 0) {
    command << " --limit " << limit;
  }
  if (dry_run) {
    command << " --dry-run";
  }
  return command.str();
}

int DownloadClient::resolve_limit(int limit_override) const {
  if (limit_override >= 0) {
    return limit_override;
  }
  return config_.limit;
}

std::string DownloadClient::download_script_path() const {
  if (const char *env_path = std::getenv("BGPSTREAM_DOWNLOAD_SCRIPT")) {
    if (*env_path != '\0') {
      return env_path;
    }
  }

  const std::filesystem::path configured = BGPSTREAM_DOWNLOAD_SCRIPT_PATH;
  if (std::filesystem::exists(configured)) {
    return configured.string();
  }

  const std::filesystem::path source_relative =
      std::filesystem::path(BGPSTREAM_SOURCE_DIR) / "python" / "download.py";
  if (std::filesystem::exists(source_relative)) {
    return source_relative.string();
  }

  const std::filesystem::path cwd_relative =
      std::filesystem::path("python") / "download.py";
  if (std::filesystem::exists(cwd_relative)) {
    return cwd_relative.string();
  }

  throw std::runtime_error("Unable to locate python/download.py");
}

std::string DownloadClient::python_executable() const {
  if (const char *env_python = std::getenv("BGPSTREAM_PYTHON")) {
    if (*env_python != '\0') {
      return env_python;
    }
  }

  if (const char *virtual_env = std::getenv("VIRTUAL_ENV")) {
    const auto candidate =
        std::filesystem::path(virtual_env) / "bin" / "python";
    if (std::filesystem::exists(candidate)) {
      return candidate.string();
    }
  }

  const auto repo_venv =
      std::filesystem::path(BGPSTREAM_SOURCE_DIR) / ".venv" / "bin" / "python";
  if (std::filesystem::exists(repo_venv)) {
    return repo_venv.string();
  }

  return "python3";
}

} // namespace bgpstream_runner
