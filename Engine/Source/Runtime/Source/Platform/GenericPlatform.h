#pragma once

// Yes, we do define these types just to remove the "_t" from stdint.h... get over it.

#include <cstdint>

using uint8  = unsigned char;
using uint16 = unsigned short int;
using uint32 = unsigned int;

using uint64 = uint64_t;
using UINTPTR = uint64;
using SIZE_T = uint64; // Windows SDK SIZE_T (ULONG_PTR on x64); size_t everywhere else.

using int8   = signed char;
using int16  = signed short int;
using int32  = signed int;
using int64  = int64_t;

namespace Lumina
{
    using int8   = signed char;
    using int16  = signed short int;
    using int32  = signed int;
    using int64  = int64_t;
    using uint8  = unsigned char;
    using uint16 = unsigned short int;
    using uint32 = unsigned int;
    using uint64 = uint64_t;
}

using ANSICHAR = char;
using WIDECHAR = wchar_t;

// Encoding-intent aliases. The engine treats narrow char buffers (ANSICHAR/UTF8CHAR) as UTF-8 and
using UTF8CHAR  = char;
using UTF16CHAR = char16_t;
using UTF32CHAR = char32_t;

// Width of a single WIDECHAR in bytes: 2 on Windows (UTF-16), 4 on most Unix platforms (UTF-32).
#define PLATFORM_WIDECHAR_SIZE sizeof(WIDECHAR)

#if defined(LE_PLATFORM_WINDOWS)
    using TCHAR = WIDECHAR;
    #define PLATFORM_TCHAR_IS_WIDE 1
#else
    using TCHAR = ANSICHAR;
    #define PLATFORM_TCHAR_IS_WIDE 0
#endif

// For a class whose vtable crosses a shared library boundary. Empty on Windows, where MSVC emits a
// vtable in every translation unit that needs one.
#if defined(LE_PLATFORM_WINDOWS)
    #define LUMINA_VISIBLE_TYPE
#else
    #define LUMINA_VISIBLE_TYPE __attribute__((visibility("default")))
#endif

// TEXT() used to arrive only because <windows.h> leaked in transitively through a third-party
// header chain. DECLARE_CLASS depends on it, so own it here instead.
#ifndef __TEXT
    #ifdef UNICODE
        #define __TEXT(quote) L##quote
    #else
        #define __TEXT(quote) quote
    #endif
#endif
#ifndef TEXT
    #define TEXT(quote) __TEXT(quote)
#endif

static_assert(sizeof(TEXT("!")[0]) == sizeof(TCHAR),
    "TEXT() and TCHAR disagree on character width. On Windows this means UNICODE was not defined "
    "(LuminaBuildTool's WindowsPlatform supplies it); elsewhere it means UNICODE leaked in from a "
    "third-party header, which makes TEXT() wide while TCHAR is UTF-8 char.");
