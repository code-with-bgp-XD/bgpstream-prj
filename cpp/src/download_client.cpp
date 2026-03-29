#include "bgpstream_runner/download_client.h"

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <stdexcept>

#include "bgpstream_runner/common.h"

namespace bgpstream_runner {

namespace {

#ifndef BGPSTREAM_DOWNLOAD_SCRIPT_PATH
#define BGPSTREAM_DOWNLOAD_SCRIPT_PATH "python/download.py"
#endif

#ifndef BGPSTREAM_SOURCE_DIR
#define BGPSTREAM_SOURCE_DIR "."
#endif

std::uint64_t parse_expected_size_bytes(const std::string &text) {
    std::size_t parsed_length = 0;
    const unsigned long long parsed_value = std::stoull(text, &parsed_length);
    if (parsed_length != text.size()) {
        throw std::runtime_error("Invalid expected size in download manifest: " + text);
    }
    if (parsed_value > std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("Expected size is too large in download manifest: " + text);
    }
    return static_cast<std::uint64_t>(parsed_value);
}

std::vector<DownloadTarget> parse_targets_from_manifest_output(const std::string &output) {
    std::vector<DownloadTarget> targets;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("FILE\t", 0) != 0) {
            continue;
        }

        const std::size_t first_tab = line.find('\t');
        const std::size_t second_tab = line.find('\t', first_tab + 1);
        const std::size_t third_tab = line.find('\t', second_tab + 1);
        if (first_tab == std::string::npos || second_tab == std::string::npos || third_tab == std::string::npos) {
            throw std::runtime_error("Invalid dry-run manifest line from download.py: " + line);
        }

        const std::string destination_text = trim(line.substr(first_tab + 1, second_tab - first_tab - 1));
        const std::string local_path_text = trim(line.substr(second_tab + 1, third_tab - second_tab - 1));
        const std::string expected_size_text = trim(line.substr(third_tab + 1));
        if (destination_text.empty() || local_path_text.empty() || expected_size_text.empty()) {
            throw std::runtime_error("Incomplete dry-run manifest line from download.py: " + line);
        }

        targets.push_back(DownloadTarget{
            std::filesystem::path(destination_text),
            std::filesystem::path(local_path_text),
            parse_expected_size_bytes(expected_size_text),
        });
    }
    return targets;
}

}  // namespace

DownloadClient::DownloadClient(Config config) : config_(std::move(config)) {}

std::vector<DownloadTarget> DownloadClient::collect_targets(const ClosedDateRange &range, int limit_override) const {
    const CommandResult result = run_capture_command(build_download_command(range, true, limit_override, false));
    if (result.exit_code != 0) {
        throw std::runtime_error("download.py --dry-run failed with exit code " + std::to_string(result.exit_code) +
                                 "\n" + result.output);
    }
    return parse_targets_from_manifest_output(result.output);
}

void DownloadClient::download_range(const ClosedDateRange &range, int limit_override) const {
    const int exit_code = run_streaming_command(build_download_command(range, false, limit_override));
    if (exit_code != 0) {
        throw std::runtime_error("download.py failed with exit code " + std::to_string(exit_code));
    }
}

std::string DownloadClient::build_download_command(const ClosedDateRange &range, bool dry_run, int limit_override,
                                                   bool probe_size) const {
    std::ostringstream command;
    command << shell_escape(python_executable()) << " " << shell_escape(download_script_path()) << " --from-time "
            << shell_escape(format_utc_timestamp(range.start_epoch)) << " --until-time "
            << shell_escape(format_utc_timestamp(range.end_exclusive_epoch)) << " --collector "
            << shell_escape(config_.collector) << " --project " << shell_escape(config_.project)
            << " --record-type updates"
            << " --source auto"
            << " --workers " << config_.download_workers << " --output-dir "
            << shell_escape(config_.output_dir.string());

    const int limit = resolve_limit(limit_override);
    if (limit > 0) {
        command << " --limit " << limit;
    }
    if (dry_run) {
        command << " --dry-run --dry-run-format tsv";
        if (probe_size) {
            command << " --probe-size";
        }
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

    const std::filesystem::path cwd_relative = std::filesystem::path("python") / "download.py";
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
        const auto candidate = std::filesystem::path(virtual_env) / "bin" / "python";
        if (std::filesystem::exists(candidate)) {
            return candidate.string();
        }
    }

    const auto repo_venv = std::filesystem::path(BGPSTREAM_SOURCE_DIR) / ".venv" / "bin" / "python";
    if (std::filesystem::exists(repo_venv)) {
        return repo_venv.string();
    }

    return "python3";
}

}  // namespace bgpstream_runner
