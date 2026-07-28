// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#ifndef SPDLOG_COMPILED_LIB
    #error Please define SPDLOG_COMPILED_LIB to compile this file.
#endif

#include <spdlog/common-inl.h>
#include <spdlog/details/backtracer-inl.h>
#include <spdlog/details/log_msg-inl.h>
#include <spdlog/details/log_msg_buffer-inl.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/details/os-inl.h>
#include <spdlog/details/periodic_worker-inl.h>
#include <spdlog/details/registry-inl.h>
#include <spdlog/logger-inl.h>
#include <spdlog/pattern_formatter-inl.h>
#include <spdlog/sinks/base_sink-inl.h>
#include <spdlog/sinks/sink-inl.h>
#include <spdlog/spdlog-inl.h>

#include <mutex>

// base_sink is only declared under SPDLOG_COMPILED_LIB; instantiate the two
// mutex flavors here so derived sinks (e.g. Lumina's FConsoleSink) link.
template class SPDLOG_API spdlog::sinks::base_sink<std::mutex>;
template class SPDLOG_API spdlog::sinks::base_sink<spdlog::details::null_mutex>;
