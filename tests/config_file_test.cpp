#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "bgpstream_runner/config_file.h"

namespace {

using namespace bgpstream_runner;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path make_temp_directory() {
    const std::string pattern =
        (std::filesystem::temp_directory_path() / "bgpstream-config-test-XXXXXX").string();
    std::vector<char> writable_pattern(pattern.begin(), pattern.end());
    writable_pattern.push_back('\0');
    char *const created = ::mkdtemp(writable_pattern.data());
    if (created == nullptr) {
        throw std::system_error(errno, std::generic_category(), "mkdtemp failed");
    }
    return created;
}

void write_config(const std::filesystem::path &path, const std::string &text) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to create test config: " + path.string());
    }
    output << text;
}

}  // namespace

int main() {
    const std::filesystem::path test_root = make_temp_directory();
    const std::filesystem::path config_path = test_root / "config.json";

    try {
        write_config(config_path,
                     R"({"analysis":{"parse_on_cache_miss":true,)"
                     R"("persist_realtime_parsed_cache":true},"cache":{"output_dir":"data"}})");
        Config enabled;
        apply_json_config_file(config_path, &enabled);
        require(enabled.parse_on_cache_miss,
                "analysis.parse_on_cache_miss=true was not applied");
        require(enabled.persist_realtime_parsed_cache,
                "analysis.persist_realtime_parsed_cache=true was not applied");

        write_config(config_path,
                     R"({"analysis":{},"cache":{"output_dir":"data"}})");
        Config default_config;
        apply_json_config_file(config_path, &default_config);
        require(!default_config.parse_on_cache_miss,
                "analysis.parse_on_cache_miss did not default to false");
        require(!default_config.persist_realtime_parsed_cache,
                "analysis.persist_realtime_parsed_cache did not default to false");

        write_config(config_path,
                     R"({"analysis":{"parse_on_cache_miss":"true"},"cache":{"output_dir":"data"}})");
        bool wrong_type_rejected = false;
        try {
            Config invalid;
            apply_json_config_file(config_path, &invalid);
        } catch (const std::runtime_error &error) {
            wrong_type_rejected =
                std::string(error.what()).find("parse_on_cache_miss") != std::string::npos;
        }
        require(wrong_type_rejected,
                "non-boolean analysis.parse_on_cache_miss was accepted");

        write_config(
            config_path,
            R"({"analysis":{"persist_realtime_parsed_cache":"true"},"cache":{"output_dir":"data"}})");
        bool persist_wrong_type_rejected = false;
        try {
            Config invalid;
            apply_json_config_file(config_path, &invalid);
        } catch (const std::runtime_error &error) {
            persist_wrong_type_rejected =
                std::string(error.what()).find("persist_realtime_parsed_cache") != std::string::npos;
        }
        require(persist_wrong_type_rejected,
                "non-boolean analysis.persist_realtime_parsed_cache was accepted");

        write_config(config_path,
                     R"({"analysis":{"max_cache_size_gb":5.0},"cache":{"output_dir":"data"}})");
        bool removed_key_rejected = false;
        try {
            Config invalid;
            apply_json_config_file(config_path, &invalid);
        } catch (const std::runtime_error &error) {
            removed_key_rejected =
                std::string(error.what()).find("Unknown config key 'analysis.max_cache_size_gb'") !=
                std::string::npos;
        }
        require(removed_key_rejected,
                "removed analysis.max_cache_size_gb key was still accepted");

        std::filesystem::remove_all(test_root);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(test_root, cleanup_error);
        return 1;
    }
}
