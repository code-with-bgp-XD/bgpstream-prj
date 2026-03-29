#pragma once

#include <filesystem>

#include "bgpstream_runner/types.h"

namespace bgpstream_runner {

void apply_json_config_file(const std::filesystem::path &path, Config *config);

}  // namespace bgpstream_runner
