#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"
#include "bgpstream_runner/types.h"

namespace bgpstream_runner {

class LoadedProcessorPlugin {
   public:
    LoadedProcessorPlugin(const Config &config, std::string_view argv0);
    ~LoadedProcessorPlugin();

    LoadedProcessorPlugin(const LoadedProcessorPlugin &) = delete;
    LoadedProcessorPlugin &operator=(const LoadedProcessorPlugin &) = delete;

    MessageProcessor &processor() const;
    const std::filesystem::path &plugin_path() const;

   private:
    std::filesystem::path resolve_plugin_path(const Config &config, std::string_view argv0) const;
    void close_handle();

    void *handle_ = nullptr;
    DestroyProcessorFn destroy_processor_ = nullptr;
    std::unique_ptr<MessageProcessor, DestroyProcessorFn> processor_{nullptr, nullptr};
    std::filesystem::path plugin_path_;
};

}  // namespace bgpstream_runner
