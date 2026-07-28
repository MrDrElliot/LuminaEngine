// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#ifndef SPDLOG_COMPILED_LIB
    #error Please define SPDLOG_COMPILED_LIB to compile this file.
#endif

// periodic_worker-inl.h is compiled by spdlog.cpp (registry needs it); including it here too
// defines those symbols in both objs and the linker warns LNK4006.
#include <spdlog/async.h>
#include <spdlog/async_logger-inl.h>
#include <spdlog/details/thread_pool-inl.h>
