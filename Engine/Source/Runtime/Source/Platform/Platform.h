#pragma once

#ifndef REFLECTION_PARSER

/** Branch prediction hints */
#ifndef LIKELY						/* Hints compiler that expression is likely to be true */
    #if defined(__clang__) || defined(__GNUC__)
        #define LIKELY(x)			__builtin_expect(!!(x), 1)
    #else
        // MSVC has no expression form; LUMINA_LIKELY_IF below is what carries the hint there.
        #define LIKELY(x)			(!!(x))
    #endif
#endif

#ifndef UNLIKELY					/* Hints compiler that expression is unlikely to be true, allows (penalized by worse performance) expression to be true */
    #if defined(__clang__) || defined(__GNUC__)
        #define UNLIKELY(x)			__builtin_expect(!!(x), 0)
    #else
        #define UNLIKELY(x)			(!!(x))
    #endif
#endif

// The statement form, which is the only branch hint MSVC understands. Takes an else like a plain if.
#if defined(__has_cpp_attribute)
    #if __has_cpp_attribute(likely) >= 201803L
        #define LUMINA_HAS_BRANCH_ATTRIBUTES 1
    #endif
#endif

#ifndef LUMINA_HAS_BRANCH_ATTRIBUTES
    #define LUMINA_HAS_BRANCH_ATTRIBUTES 0
#endif

#if LUMINA_HAS_BRANCH_ATTRIBUTES
    #define LUMINA_LIKELY_IF(Condition)   if (Condition) [[likely]]
    #define LUMINA_UNLIKELY_IF(Condition) if (Condition) [[unlikely]]
#else
    #define LUMINA_LIKELY_IF(Condition)   if (LIKELY(Condition))
    #define LUMINA_UNLIKELY_IF(Condition) if (UNLIKELY(Condition))
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

// A forced inline the target may demote, for code too big to be worth forcing everywhere.
#ifndef LUMINA_FORCEINLINE_HINTS_FORCED
    #define LUMINA_FORCEINLINE_HINTS_FORCED 1
#endif

#if LUMINA_FORCEINLINE_HINTS_FORCED
    #define LUMINA_FORCEINLINE_HINT FORCEINLINE
#else
    #define LUMINA_FORCEINLINE_HINT inline
#endif

// Forced everywhere but Debug, where the debugger steps over a forced inline rather than into it.
#if defined(LE_DEBUG)
    #define LUMINA_FORCEINLINE_DEBUGGABLE inline
#else
    #define LUMINA_FORCEINLINE_DEBUGGABLE FORCEINLINE
#endif

// Credits an allocation to its caller in the heap profiler, claiming nothing about aliasing.
#if defined(_MSC_VER)
    #define LUMINA_ALLOCATION __declspec(allocator)
    #define LUMINA_NOALIAS    __declspec(noalias)
#else
    #define LUMINA_ALLOCATION
    #define LUMINA_NOALIAS
#endif

// The returned pointer aliases nothing already live, which a reallocation cannot promise.
#if defined(_MSC_VER)
    #define LUMINA_RESTRICT_RETURN __declspec(restrict)
#elif defined(__GNUC__) || defined(__clang__)
    #define LUMINA_RESTRICT_RETURN __attribute__((malloc))
#else
    #define LUMINA_RESTRICT_RETURN
#endif

// Argument indices are 1 based, and count the implicit this on a member function.
#if defined(__GNUC__) || defined(__clang__)
    #define LUMINA_ALLOC_SIZE(SizeArg)                __attribute__((alloc_size(SizeArg)))
    #define LUMINA_ALLOC_SIZE_2(CountArg, SizeArg)    __attribute__((alloc_size(CountArg, SizeArg)))
    #define LUMINA_ASSUME_ALIGNED(Alignment)          __attribute__((assume_aligned(Alignment)))
    #define LUMINA_RETURNS_NONNULL                    __attribute__((returns_nonnull))
    #define LUMINA_HOT                                __attribute__((hot))
    #define LUMINA_COLD                               __attribute__((cold))
#else
    #define LUMINA_ALLOC_SIZE(SizeArg)
    #define LUMINA_ALLOC_SIZE_2(CountArg, SizeArg)
    #define LUMINA_ASSUME_ALIGNED(Alignment)
    #define LUMINA_RETURNS_NONNULL
    #define LUMINA_HOT
    #define LUMINA_COLD
#endif

// Diagnoses a returned reference or view that outlives the temporary it was taken from.
#if defined(__clang__)
    #define LUMINA_LIFETIMEBOUND [[clang::lifetimebound]]
#else
    #define LUMINA_LIFETIMEBOUND
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
#define LUMINA_HAS_BRANCH_ATTRIBUTES 0
#define LUMINA_LIKELY_IF(Condition)   if (Condition)
#define LUMINA_UNLIKELY_IF(Condition) if (Condition)
#define LE_CONSTEVAL constexpr
#define NODISCARD
#define ALIGNOF(type) alignof(type)
#define ALIGN(n)
#define CACHE_LINE_SIZE 64
#define LUMINA_NOVTABLE
#define FORCEINLINE inline
#define FORCENOINLINE

#define LUMINA_FORCEINLINE_HINTS_FORCED 1
#define LUMINA_FORCEINLINE_HINT inline
#define LUMINA_FORCEINLINE_DEBUGGABLE inline
#define LUMINA_ALLOCATION
#define LUMINA_NOALIAS
#define LUMINA_RESTRICT_RETURN
#define LUMINA_ALLOC_SIZE(SizeArg)
#define LUMINA_ALLOC_SIZE_2(CountArg, SizeArg)
#define LUMINA_ASSUME_ALIGNED(Alignment)
#define LUMINA_RETURNS_NONNULL
#define LUMINA_HOT
#define LUMINA_COLD
#define LUMINA_LIFETIMEBOUND

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
