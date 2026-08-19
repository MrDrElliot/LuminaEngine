#pragma once
#include <format>
#include <source_location>
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"


#if __has_include(<stacktrace>) && defined(__cpp_lib_stacktrace)
#include <stacktrace>
#define HAS_STD_STACKTRACE 1
#else
#define HAS_STD_STACKTRACE 0
#endif

#if defined(_MSC_VER)
    extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent();
    // Breaks only under a debugger, so a failing assertion still reaches its handler in a normal run.
    #define LUMINA_DEBUG_BREAK() do { if (::IsDebuggerPresent()) { __debugbreak(); } } while (false)
#else
    #define LUMINA_DEBUG_BREAK() ((void)0)
#endif

namespace Lumina::Assert
{
    enum class EAssertionType : uint8
    {
        DebugAssert,
        Assert,
        Assume,
        Unreachable,
        Panic,
        Alert,
    };
    
    struct RUNTIME_API FAssertion
    {
        FString                 Message;
        std::source_location    Location;
        FStringView             Expression;
        EAssertionType          Type;
    };
    
    
    using AssertionHandler = void(*)(const FAssertion&);
    
    RUNTIME_API void SetAssertionHandler(AssertionHandler Handler);
    
    namespace Detail
    {
        FORCENOINLINE RUNTIME_API void HandleAssertion(const FAssertion& Assertion);
        [[noreturn]] FORCENOINLINE RUNTIME_API void Abort();
        
        constexpr bool ShouldAbortOnAssertion(EAssertionType Type)
        {
            switch (Type)
            {
                case EAssertionType::Assert:        return true;
                case EAssertionType::Assume:        return true;
                case EAssertionType::DebugAssert:   return true;
                case EAssertionType::Unreachable:   return true;
                case EAssertionType::Panic:         return true;
                case EAssertionType::Alert:         return false;
                default:                            return true;
            }
        }
    }
    
    // ASSERT_DEBUG: debug/dev only. ASSERT: all configs. ASSUME: dev-checked, shipping optimizer hint.
    
#define LUMINA_HANDLE_ASSERTION_HEADER \
    ::Lumina::Assert::Detail::HandleAssertion(FAssertion

#define LUMINA_ASSERTION_BODY(Expr, AssertType, ...) \
    if(!(Expr)) [[unlikely]] \
    { \
        LUMINA_DEBUG_BREAK(); \
        using namespace Lumina::Assert; \
        LUMINA_HANDLE_ASSERTION_HEADER \
        { \
            __VA_OPT__(.Message = std::format(__VA_ARGS__).c_str(),) \
            .Location = std::source_location::current(), \
            .Expression = #Expr, \
            .Type = EAssertionType::AssertType \
        }); \
        if (::Lumina::Assert::Detail::ShouldAbortOnAssertion(EAssertionType::AssertType)) \
        { \
            ::Lumina::Assert::Detail::Abort(); \
        } \
    } \

#define LUMINA_ASSERT_INVOKE(Expr, Type, ...) \
    do \
    { \
        LUMINA_ASSERTION_BODY(Expr, Type, __VA_ARGS__) \
    } while(0)

#define LUMINA_ALERT_IF_NOT_INVOKE(Expr, ...) \
    [&]() \
    { \
        auto _LuminaAlertResult_ = (Expr); \
        if (!_LuminaAlertResult_) [[unlikely]] \
        { \
            LUMINA_DEBUG_BREAK(); \
            using namespace Lumina::Assert; \
            LUMINA_HANDLE_ASSERTION_HEADER \
            { \
                __VA_OPT__(.Message = std::format(__VA_ARGS__).c_str(),) \
                .Location = std::source_location::current(), \
                .Expression = #Expr, \
                .Type = EAssertionType::Alert \
            }); \
            if (::Lumina::Assert::Detail::ShouldAbortOnAssertion(EAssertionType::Alert)) \
            { \
                ::Lumina::Assert::Detail::Abort(); \
            } \
        } \
        return _LuminaAlertResult_; \
    }()

#define LUMINA_ALERT_IF_INVOKE(Expr, ...) \
    [&]() \
    { \
        auto _LuminaAlertResult_ = (Expr); \
        if (_LuminaAlertResult_) [[unlikely]] \
        { \
            LUMINA_DEBUG_BREAK(); \
            using namespace Lumina::Assert; \
            LUMINA_HANDLE_ASSERTION_HEADER \
            { \
                __VA_OPT__(.Message = std::format(__VA_ARGS__).c_str(),) \
                .Location = std::source_location::current(), \
                .Expression = #Expr, \
                .Type = EAssertionType::Alert \
            }); \
            if (::Lumina::Assert::Detail::ShouldAbortOnAssertion(EAssertionType::Alert)) \
            { \
                ::Lumina::Assert::Detail::Abort(); \
            } \
        } \
        return _LuminaAlertResult_; \
    }()
    

#if defined(LE_DEBUG) || defined(LE_DEVELOPMENT)
#define LUMINA_DEBUG_ASSERT(Expr, ...)  LUMINA_ASSERT_INVOKE(Expr,      DebugAssert,    __VA_ARGS__)
#else
#define LUMINA_DEBUG_ASSERT(...)        ((void)0)
#endif
    
#define LUMINA_ASSERT(Expr, ...)        LUMINA_ASSERT_INVOKE(Expr,      DebugAssert,    __VA_ARGS__)


#if defined(LE_DEBUG) || defined(LE_DEVELOPMENT)
#define LUMINA_ASSUME(Expr, ...)        LUMINA_ASSERT_INVOKE(Expr,      Assume,         __VA_ARGS__)
#else
#ifdef _MSC_VER
#define LUMINA_ASSUME(Condition, ...)   __assume(Condition)
#elif defined(__clang__)
#define LUMINA_ASSUME(Condition, ...)   __builtin_assume(Condition)
#elif defined(__GNUC__)
#define LUMINA_ASSUME(Condition, ...)   do { if (!(Condition)) __builtin_unreachable(); } while(0)
#else
#define LUMINA_ASSUME(Condition, ...)   ((void)0)
#endif
#endif
    

#define LUMINA_PANIC(...)                       LUMINA_ASSERT_INVOKE(false,             Panic,          __VA_ARGS__)
    
#ifndef LE_SHIPPING
#define LUMINA_ALERT_IF_NOT(Expr, ...)          LUMINA_ALERT_IF_NOT_INVOKE(Expr,                        __VA_ARGS__)
#define LUMINA_ALERT_IF(Expr, ...)              LUMINA_ALERT_IF_INVOKE(Expr,                            __VA_ARGS__)
#else
#define LUMINA_ALERT_IF_NOT(Expr, ...)          Expr
#define LUMINA_ALERT_IF(Expr, ...)              Expr
#endif
    
#ifdef LE_DEBUG
#define LUMINA_UNREACHABLE(...)         LUMINA_ASSERT_INVOKE(false,     Unreachable,    __VA_ARGS__); std::unreachable()
#else
#define LUMINA_UNREACHABLE(...)         std::unreachable()
#endif
    
    
    
#define DEBUG_ASSERT(...)   LUMINA_DEBUG_ASSERT(__VA_ARGS__)
#define ASSERT(...)         LUMINA_ASSERT(__VA_ARGS__)
#define ASSUME(...)         LUMINA_ASSUME(__VA_ARGS__)
#define ALERT_IF_NOT(...)   LUMINA_ALERT_IF_NOT(__VA_ARGS__)
#define ALERT_IF(...)       LUMINA_ALERT_IF(__VA_ARGS__)
#define PANIC(...)          LUMINA_PANIC(__VA_ARGS__)
#define UNREACHABLE(...)    LUMINA_UNREACHABLE(__VA_ARGS__)

    

    
    
}
