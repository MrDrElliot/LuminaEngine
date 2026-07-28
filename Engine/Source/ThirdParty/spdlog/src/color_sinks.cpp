// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#ifndef SPDLOG_COMPILED_LIB
    #error Please define SPDLOG_COMPILED_LIB to compile this file.
#endif

// logger.h + registry.h are needed in full: synchronous_factory::create constructs a logger.
#include <spdlog/details/null_mutex.h>
#include <spdlog/details/registry.h>
#include <spdlog/details/synchronous_factory.h>
#include <spdlog/logger.h>

#include <mutex>

#ifdef _WIN32
    #include <spdlog/sinks/wincolor_sink-inl.h>

template class SPDLOG_API spdlog::sinks::wincolor_sink<spdlog::details::console_mutex>;
template class SPDLOG_API spdlog::sinks::wincolor_sink<spdlog::details::console_nullmutex>;
template class SPDLOG_API spdlog::sinks::wincolor_stdout_sink<spdlog::details::console_mutex>;
template class SPDLOG_API spdlog::sinks::wincolor_stdout_sink<spdlog::details::console_nullmutex>;
template class SPDLOG_API spdlog::sinks::wincolor_stderr_sink<spdlog::details::console_mutex>;
template class SPDLOG_API spdlog::sinks::wincolor_stderr_sink<spdlog::details::console_nullmutex>;
#else
    #include <spdlog/sinks/ansicolor_sink-inl.h>

template class SPDLOG_API spdlog::sinks::ansicolor_sink<spdlog::details::console_mutex>;
template class SPDLOG_API spdlog::sinks::ansicolor_sink<spdlog::details::console_nullmutex>;
template class SPDLOG_API spdlog::sinks::ansicolor_stdout_sink<spdlog::details::console_mutex>;
template class SPDLOG_API spdlog::sinks::ansicolor_stdout_sink<spdlog::details::console_nullmutex>;
template class SPDLOG_API spdlog::sinks::ansicolor_stderr_sink<spdlog::details::console_mutex>;
template class SPDLOG_API spdlog::sinks::ansicolor_stderr_sink<spdlog::details::console_nullmutex>;
#endif

#include <spdlog/sinks/stdout_color_sinks-inl.h>

template SPDLOG_API std::shared_ptr<spdlog::logger> spdlog::stdout_color_mt<spdlog::synchronous_factory>(
    const std::string &logger_name, spdlog::color_mode mode);
template SPDLOG_API std::shared_ptr<spdlog::logger> spdlog::stdout_color_st<spdlog::synchronous_factory>(
    const std::string &logger_name, spdlog::color_mode mode);
template SPDLOG_API std::shared_ptr<spdlog::logger> spdlog::stderr_color_mt<spdlog::synchronous_factory>(
    const std::string &logger_name, spdlog::color_mode mode);
template SPDLOG_API std::shared_ptr<spdlog::logger> spdlog::stderr_color_st<spdlog::synchronous_factory>(
    const std::string &logger_name, spdlog::color_mode mode);
