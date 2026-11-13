#pragma once

#if defined(_WIN32)
    #define LRT_EXPORT __declspec(dllexport)
    #define LRT_IMPORT __declspec(dllimport)
#else
    #define LRT_EXPORT __attribute__((visibility("default")))
    #define LRT_IMPORT
#endif

// The Reflector executable exports the generator API, and a Reflection module's library imports it.
#if defined(LRT_HOST)
    #define LRT_API LRT_EXPORT
#else
    #define LRT_API LRT_IMPORT
#endif
