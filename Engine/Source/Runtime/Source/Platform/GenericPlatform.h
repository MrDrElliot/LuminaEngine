#pragma once

// Yes, we do define these types just to remove the "_t" from stdint.h... get over it.

using uint8  = unsigned char;
using uint16 = unsigned short int;
using uint32 = unsigned int;
using uint64 = unsigned long long;
using UINTPTR = uint64;
using SIZE_T = uint64; // Windows SDK SIZE_T (ULONG_PTR on x64).

using int8   = signed char;
using int16  = signed short int;
using int32  = signed int;
using int64  = signed long long;

// Reflector workaround: the LRT amalgamation parse (libclang) mis-resolves the GLOBAL ::int32/::uint32 typedefs
// when they appear as container template arguments (e.g. THashMap<int32, int32>), silently recovering the field
// to `int` so the reflected property comes out wrong. Namespace-scoped mirrors make intN in Lumina code resolve
// here, which libclang handles correctly. Same underlying types, so this is transparent to normal compilation.
namespace Lumina
{
    using int8   = signed char;
    using int16  = signed short int;
    using int32  = signed int;
    using int64  = signed long long;
    using uint8  = unsigned char;
    using uint16 = unsigned short int;
    using uint32 = unsigned int;
    using uint64 = unsigned long long;
}

using ANSICHAR = char;
using WIDECHAR = wchar_t;
using TCHAR = WIDECHAR; // Switchable character; either ANSICHAR or WIDECHAR.

// Encoding-intent aliases. The engine treats narrow char buffers (ANSICHAR/UTF8CHAR) as UTF-8 and
// wide buffers (WIDECHAR/TCHAR) as UTF-16 on Windows. See PlatformString.h / StringCast.
using UTF8CHAR  = char;
using UTF16CHAR = char16_t;
using UTF32CHAR = char32_t;

// Width of a single WIDECHAR in bytes: 2 on Windows (UTF-16), 4 on most Unix platforms (UTF-32).
#define PLATFORM_WIDECHAR_SIZE sizeof(WIDECHAR)

// TEXT() used to arrive only because <windows.h> leaked in transitively through a third-party
// header chain. DECLARE_CLASS depends on it, so own it here instead.
// Token-for-token identical to winnt.h's definition, so windows.h redefining it later is a
// legal identical redefinition rather than a C4005 conflict.
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
