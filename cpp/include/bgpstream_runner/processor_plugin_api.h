#pragma once

#include "bgpstream_runner/message_processor.h"

namespace bgpstream_runner {

using CreateProcessorFn = MessageProcessor *(*)();
using DestroyProcessorFn = void (*)(MessageProcessor *);

inline constexpr char kCreateProcessorSymbol[] = "bgpstream_create_processor";
inline constexpr char kDestroyProcessorSymbol[] = "bgpstream_destroy_processor";

}  // namespace bgpstream_runner

#define BGPSTREAM_RUNNER_EXPORT_PROCESSOR(PROCESSOR_TYPE)                                 \
    extern "C" bgpstream_runner::MessageProcessor *bgpstream_create_processor() {         \
        return new PROCESSOR_TYPE();                                                      \
    }                                                                                     \
    extern "C" void bgpstream_destroy_processor(bgpstream_runner::MessageProcessor *p) {  \
        delete p;                                                                         \
    }
