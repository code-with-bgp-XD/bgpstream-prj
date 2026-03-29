#pragma once

#include "bgpstream_runner/types.h"

#include <filesystem>

namespace bgpstream_runner {

void apply_json_config_file(
    const std::filesystem::path &path,
    Config *config);

}  // namespace bgpstream_runner
