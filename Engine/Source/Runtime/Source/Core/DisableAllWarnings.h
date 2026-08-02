#pragma once

// Disables compiler warnings for Clang/GCC/MSVC; wrap third-party includes between
// PRAGMA_DISABLE_ALL_WARNINGS and the matching restore macro.


#if defined(__clang__)
    #define PRAGMA_DISABLE_ALL_WARNINGS     \
        _Pragma("clang diagnostic push")    \
        _Pragma("clang diagnostic ignored \"-Weverything\"")

    #define PRAGMA_ENABLE_ALL_WARNINGS      \
        _Pragma("clang diagnostic pop")

#elif defined(__GNUC__)
    #define PRAGMA_DISABLE_ALL_WARNINGS                                \
        _Pragma("GCC diagnostic push")                                 \
        _Pragma("GCC diagnostic ignored \"-Wall\"")                    \
        _Pragma("GCC diagnostic ignored \"-Wextra\"")                  \
        _Pragma("GCC diagnostic ignored \"-Wpragmas\"")                \
        _Pragma("GCC diagnostic ignored \"-Wshadow\"")                 \
        _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"")       \
        _Pragma("GCC diagnostic ignored \"-Wconversion\"")             \
        _Pragma("GCC diagnostic ignored \"-Wsign-conversion\"")        \
        _Pragma("GCC diagnostic ignored \"-Wsign-compare\"")           \
        _Pragma("GCC diagnostic ignored \"-Wfloat-conversion\"")       \
        _Pragma("GCC diagnostic ignored \"-Wswitch-enum\"")            \
        _Pragma("GCC diagnostic ignored \"-Wswitch\"")                 \
        _Pragma("GCC diagnostic ignored \"-Wzero-as-null-pointer-constant\"") \
        _Pragma("GCC diagnostic ignored \"-Wunused-variable\"")        \
        _Pragma("GCC diagnostic ignored \"-Wredundant-decls\"")        \
        _Pragma("GCC diagnostic ignored \"-Wsubobject-linkage\"")      \
        _Pragma("GCC diagnostic ignored \"-Wunused-but-set-variable\"")\
        _Pragma("GCC diagnostic ignored \"-Wredundant-move\"")         \
        _Pragma("GCC diagnostic ignored \"-Wstrict-aliasing\"")        \
        _Pragma("GCC diagnostic ignored \"-Woverloaded-virtual\"")     \
        _Pragma("GCC diagnostic ignored \"-Wc99-extensions\"")         \
        _Pragma("GCC diagnostic ignored \"-Wmisleading-indentation\"") \
        _Pragma("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")    \
        _Pragma("GCC diagnostic ignored \"-Wimplicit-fallthrough\"")   \
        _Pragma("GCC diagnostic ignored \"-Wcast-function-type\"")     \
        _Pragma("GCC diagnostic ignored \"-Wunused-label\"")           \
        _Pragma("GCC diagnostic ignored \"-Wnarrowing\"")              \
        _Pragma("GCC diagnostic ignored \"-Wparentheses\"")            \
        _Pragma("GCC diagnostic ignored \"-Wwrite-strings\"")          \
        _Pragma("GCC diagnostic ignored \"-Wformat-overflow\"")        \
        _Pragma("GCC diagnostic ignored \"-Wdeprecated-copy-with-dtor\"") \
        _Pragma("GCC diagnostic ignored \"-Wdeprecated\"")             \
        _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")\
        _Pragma("GCC diagnostic ignored \"-Wuse-after-free\"")         \
        _Pragma("GCC diagnostic ignored \"-Warray-bounds\"")           \
        _Pragma("GCC diagnostic ignored \"-Wvolatile\"")               \
        _Pragma("GCC diagnostic ignored \"-Wmissing-field-initializers\"") \
        _Pragma("GCC diagnostic ignored \"-Wfloat-equal\"")            \
        _Pragma("GCC diagnostic ignored \"-Wpedantic\"")               \
        _Pragma("GCC diagnostic ignored \"-Wvla\"")

    #define PRAGMA_ENABLE_ALL_WARNINGS  \
        _Pragma("GCC diagnostic pop")

#elif defined(_MSC_VER)
    #define PRAGMA_DISABLE_ALL_WARNINGS      \
        __pragma(warning(push, 0))           \

    #define PRAGMA_ENABLE_ALL_WARNINGS \
        __pragma(warning(pop))

#else
    #define PRAGMA_DISABLE_ALL_WARNINGS
    #define PRAGMA_ENABLE_ALL_WARNINGS
#endif