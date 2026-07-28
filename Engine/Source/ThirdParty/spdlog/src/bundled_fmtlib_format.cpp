// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

// Compiles the bundled fmtlib once. Without SPDLOG_COMPILED_LIB, spdlog/fmt/fmt.h
// defines FMT_HEADER_ONLY and every TU parses format-inl.h instead.

#ifndef SPDLOG_COMPILED_LIB
    #error Please define SPDLOG_COMPILED_LIB to compile this file.
#endif

#if !defined(SPDLOG_FMT_EXTERNAL)
    #include <spdlog/fmt/bundled/format-inl.h>
#endif
