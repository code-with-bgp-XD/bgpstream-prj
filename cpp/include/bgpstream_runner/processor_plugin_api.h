#pragma once

#include <cstdint>

#include "bgpstream_runner/message_processor.h"

namespace bgpstream_runner {

inline constexpr std::uint32_t kProcessorPluginAPIVersion = 4;

using ProcessorAPIVersionFn = std::uint32_t (*)();
using CreateProcessorFn = MessageProcessor *(*)();
using DestroyProcessorFn = void (*)(MessageProcessor *);

inline constexpr char kProcessorAPIVersionSymbol[] = "bgpstream_processor_api_version";
inline constexpr char kCreateProcessorSymbol[] = "bgpstream_create_processor";
inline constexpr char kDestroyProcessorSymbol[] = "bgpstream_destroy_processor";

}  // namespace bgpstream_runner

#define BGPSTREAM_RUNNER_EXPORT_PROCESSOR(PROCESSOR_TYPE)                                       \
    extern "C" std::uint32_t bgpstream_processor_api_version() {                               \
        return bgpstream_runner::kProcessorPluginAPIVersion;                                    \
    }                                                                                           \
    extern "C" bgpstream_runner::MessageProcessor *bgpstream_create_processor() {               \
        return new PROCESSOR_TYPE();                                                            \
    }                                                                                           \
    extern "C" void bgpstream_destroy_processor(bgpstream_runner::MessageProcessor *processor) { \
        delete processor;                                                                       \
    }
