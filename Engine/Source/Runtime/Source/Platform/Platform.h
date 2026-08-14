#pragma once

#ifndef REFLECTION_PARSER

/** Branch prediction hints */
#ifndef LIKELY						/* Hints compiler that expression is likely to be true */
    #if ( defined(__clang__) || defined(__GNUC__) ) && (PLATFORM_UNIX)	// effect of these on non-Linux platform has not been analyzed as of 2016-03-21
        #define LIKELY(x)			__builtin_expect(!!(x), 1)
    #else
        // the additional "!!" is added to silence "warning: equality comparison with extraneous parenthesis" messages on android
        #define LIKELY(x)			(!!(x))
    #endif
#endif

#ifndef UNLIKELY					/* Hints compiler that expression is unlikely to be true, allows (penalized by worse performance) expression to be true */
    #if ( defined(__clang__) || defined(__GNUC__) ) && (PLATFORM_UNIX)	// effect of these on non-Linux platform has not been analyzed as of 2016-03-21
        #define UNLIKELY(x)			__builtin_expect(!!(x), 0)
    #else
        // the additional "!!" is added to silence "warning: equality comparison with exteraneous parenthese" messages on android
        #define UNLIKELY(x)			(!!(x))
    #endif
#endif

/* Macro wrapper for the consteval keyword which isn't yet present on all compilers - constexpr
   can be used as a workaround but is less strict and so may let some non-consteval code pass */
#if defined(__cpp_consteval)
    #define LE_CONSTEVAL consteval
#else
    #define LE_CONSTEVAL constexpr
#endif

#define NODISCARD [[nodiscard]]

#ifndef ALIGNOF
    #define ALIGNOF(type) alignof(type)
#endif

#ifndef ALIGN
    #if defined(_MSC_VER)
        #define ALIGN(n) __declspec(align(n))
    #else
        #define ALIGN(n) __attribute__((aligned(n)))
    #endif
#endif

#ifndef CACHE_LINE_SIZE
    #define CACHE_LINE_SIZE 64
#endif

#if defined(_MSC_VER)
    #define LUMINA_NOVTABLE __declspec(novtable)
#else
    #define LUMINA_NOVTABLE
#endif

// Forced inline functions will be inlined in debug, and thus will be stepped over by the debugger.
#ifndef FORCEINLINE
    #if defined(_MSC_VER)
        #define FORCEINLINE __forceinline
    #else
        #define FORCEINLINE inline __attribute__((always_inline))
    #endif
#endif

#if defined(_MSC_VER)
    #define FORCENOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    #define FORCENOINLINE __attribute__((noinline))
#else
    #define FORCENOINLINE
#endif

#define UTF8TEXT_PASTE(x)  u8 ## x
#define UTF16TEXT_PASTE(x) u ## x

#ifndef PLATFORM_WIDECHAR_IS_CHAR16
    #define PLATFORM_WIDECHAR_IS_CHAR16 0
#endif

#if PLATFORM_WIDECHAR_IS_CHAR16
    #define WIDETEXT_PASTE(x)  UTF16TEXT_PASTE(x)
#else
    #define WIDETEXT_PASTE(x)  L ## x
#endif

#define UTF16TEXT(x) UTF16TEXT_PASTE(x)
#define WIDETEXT(str) WIDETEXT_PASTE(str)

#else // REFLECTION_PARSER

// libclang parses declarations, not code generation, so the attribute spellings above are noise to it
// - but the macro names still appear in reflected declarations and must expand to something, or a
// member like "FORCEINLINE FVector3 GetSize()" error-recovers into a bogus int property.

#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#define LE_CONSTEVAL constexpr
#define NODISCARD
#define ALIGNOF(type) alignof(type)
#define ALIGN(n)
#define CACHE_LINE_SIZE 64
#define LUMINA_NOVTABLE
#define FORCEINLINE inline
#define FORCENOINLINE

#define UTF8TEXT_PASTE(x)  u8 ## x
#define UTF16TEXT_PASTE(x) u ## x
#define WIDETEXT_PASTE(x)  L ## x
#define UTF16TEXT(x) UTF16TEXT_PASTE(x)
#define WIDETEXT(str) WIDETEXT_PASTE(str)

#endif

#ifndef RESTRICT
    #define RESTRICT __restrict						/* no alias hint */
#endif

#if defined(_MSC_VER)
    #define LUMINA_DISABLE_OPTIMIZATION __pragma(optimize("", off))
    #define LUMINA_ENABLE_OPTIMIZATION  __pragma(optimize("", on))
#elif defined(__clang__) || defined(__GNUC__)
    #define LUMINA_DISABLE_OPTIMIZATION _Pragma("GCC push_options") \
    _Pragma("GCC optimize(\"O0\")")
    #define LUMINA_ENABLE_OPTIMIZATION  _Pragma("GCC pop_options")
#else
    #define LUMINA_DISABLE_OPTIMIZATION
    #define LUMINA_ENABLE_OPTIMIZATION
#endif

#if defined(_MSC_VER) && defined(_M_IX86)
    #define LUMINA_STDCALL __stdcall
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__i386__)
    #define LUMINA_STDCALL __attribute__((stdcall))
#else
    #define LUMINA_STDCALL
#endif

// Generic void function pointer type
using FVoidFuncPtr = void (LUMINA_STDCALL *)();
using FVoidFuncPtrCDecl = void (*)(); // For cdecl-style (normal) functions
