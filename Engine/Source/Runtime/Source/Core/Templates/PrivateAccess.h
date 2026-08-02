#pragma once

#include <functional>
#include <type_traits>
#include <utility>

// Reach a private/protected member of any class without modifying it.
//
//
//   class FFoo
//   {
//       int  Counter = 0;
//       int  Compute(int X) const;
//       void Reset();
//       void Reset(int To);                 // overloaded
//       static inline int sCount = 0;
//       static int sBump(int By);
//       template<typename T> T Cast(int X); // template method
//   };
//
//   LUMINA_PRIVATE_MEMBER(FFoo, Counter, int);                  // Data Member
//   LUMINA_PRIVATE_MEMBER(FFoo, Compute, int(int) const);       // Method
//   LUMINA_PRIVATE_MEMBER(FFoo, Reset,   void());               // Overload: Reset()
//   LUMINA_PRIVATE_MEMBER(FFoo, Reset,   void(int));            // Overload: Reset(int)
//   LUMINA_PRIVATE_STATIC(FFoo, sCount,  int);                  // Static Data
//   LUMINA_PRIVATE_STATIC(FFoo, sBump,   int(int));             // Static Function
//   LUMINA_PRIVATE_TEMPLATE_MEMBER(FFoo, Cast, (float), float(int));  // Cast<float>
//   LUMINA_PRIVATE_TEMPLATE_STATIC(FFoo, sMake, (int), int(int));     // sMake<int>
//
//   namespace PA = Lumina::PrivateAccess;
//   FFoo Foo;
//   PA::Counter<FFoo>(Foo) = 42;          // Data -> int&
//   int R = PA::Compute<FFoo>(Foo, 5);    // Method Call
//   PA::Reset<FFoo>(Foo);                 // Resolves to Reset() at the call site
//   PA::Reset<FFoo>(Foo, 7);              // Resolves to Reset(int) at the call site
//   PA::sCount<FFoo>() = 7;               // Static Data -> int&
//   PA::sBump<FFoo>(2);                   // Static Function
//   PA::Cast<FFoo>(Foo, 3);               // Cast<float>(3)
//
// For template members, the template arguments are passed parenthesized so the
// signature may contain commas.
//

namespace Lumina::PrivateAccess
{

    template<typename TTag, typename TPtr, TPtr Ptr>
    struct TTagInstaller
    {
        friend TPtr GetMemberPtr(TTag) { return Ptr; }
    };
}


#define LUMINA_PA_CAT_(a, b, c) a##b##c
#define LUMINA_PA_CAT(a, b, c)  LUMINA_PA_CAT_(a, b, c)
#define LUMINA_PA_ESC(...)      __VA_ARGS__


#define LUMINA_PRIVATE_MEMBER(ClassType, MemberName, ...)                                    \
    LUMINA_PRIVATE_MEMBER_IMPL(LUMINA_PA_CAT(MemberName, _LmPrivTag_, __COUNTER__),          \
        ClassType, MemberName, __VA_ARGS__)
#define LUMINA_PRIVATE_MEMBER_IMPL(Tag, ClassType, MemberName, ...)                          \
    namespace PrivateAccess                                                                  \
    {                                                                                        \
        struct Tag                                                                           \
        {                                                                                    \
            using TFn  = __VA_ARGS__;                                                        \
            using TPtr = TFn ClassType::*;                                                   \
            friend TPtr GetMemberPtr(Tag);                                                   \
        };                                                                                   \
        template struct ::Lumina::PrivateAccess::TTagInstaller<Tag, Tag::TPtr,               \
            &ClassType::MemberName>;                                                         \
        template<typename TClass, typename... TArgs>                                         \
        requires (std::is_same_v<TClass, ClassType>                                          \
                   && std::is_invocable_v<Tag::TPtr, TArgs...>)                              \
        constexpr decltype(auto) MemberName(TArgs&&... Args)                                 \
        {                                                                                    \
            return std::invoke(GetMemberPtr(Tag{}), std::forward<TArgs>(Args)...);           \
        }                                                                                    \
    }


#define LUMINA_PRIVATE_STATIC(ClassType, MemberName, ...)                                    \
    LUMINA_PRIVATE_STATIC_IMPL(LUMINA_PA_CAT(MemberName, _LmPrivTag_, __COUNTER__),          \
        ClassType, MemberName, __VA_ARGS__)
#define LUMINA_PRIVATE_STATIC_IMPL(Tag, ClassType, MemberName, ...)                          \
    namespace PrivateAccess                                                                  \
    {                                                                                        \
        struct Tag                                                                           \
        {                                                                                    \
            using TFn  = __VA_ARGS__;                                                        \
            using TPtr = TFn*;                                                               \
            friend TPtr GetMemberPtr(Tag);                                                   \
        };                                                                                   \
        template struct ::Lumina::PrivateAccess::TTagInstaller<Tag, Tag::TPtr,               \
            &ClassType::MemberName>;                                                         \
        template<typename TClass, typename... TArgs>                                         \
        requires (std::is_same_v<TClass, ClassType>                                          \
                   && ((std::is_function_v<std::remove_pointer_t<Tag::TPtr>>                 \
                            && std::is_invocable_v<Tag::TPtr, TArgs...>)                      \
                       || (!std::is_function_v<std::remove_pointer_t<Tag::TPtr>>             \
                            && sizeof...(TArgs) == 0)))                                       \
        constexpr decltype(auto) MemberName(TArgs&&... Args)                                 \
        {                                                                                    \
            if constexpr (std::is_function_v<std::remove_pointer_t<Tag::TPtr>>)              \
                return std::invoke(GetMemberPtr(Tag{}), std::forward<TArgs>(Args)...);       \
            else                                                                             \
                return (*GetMemberPtr(Tag{}));                                               \
        }                                                                                    \
    }


#define LUMINA_PRIVATE_TEMPLATE_MEMBER(ClassType, MemberName, TemplateArgs, ...)             \
    LUMINA_PRIVATE_TEMPLATE_MEMBER_IMPL(LUMINA_PA_CAT(MemberName, _LmPrivTag_, __COUNTER__), \
        ClassType, MemberName, TemplateArgs, __VA_ARGS__)
#define LUMINA_PRIVATE_TEMPLATE_MEMBER_IMPL(Tag, ClassType, MemberName, TemplateArgs, ...)   \
    namespace PrivateAccess                                                                  \
    {                                                                                        \
        struct Tag                                                                           \
        {                                                                                    \
            using TFn  = __VA_ARGS__;                                                        \
            using TPtr = TFn ClassType::*;                                                   \
            friend TPtr GetMemberPtr(Tag);                                                   \
        };                                                                                   \
        template struct ::Lumina::PrivateAccess::TTagInstaller<Tag, Tag::TPtr,               \
            &ClassType::MemberName<LUMINA_PA_ESC TemplateArgs>>;                             \
        template<typename TClass, typename... TArgs>                                         \
        requires (std::is_same_v<TClass, ClassType>                                          \
                   && std::is_invocable_v<Tag::TPtr, TArgs...>)                              \
        constexpr decltype(auto) MemberName(TArgs&&... Args)                                 \
        {                                                                                    \
            return std::invoke(GetMemberPtr(Tag{}), std::forward<TArgs>(Args)...);           \
        }                                                                                    \
    }


#define LUMINA_PRIVATE_TEMPLATE_STATIC(ClassType, MemberName, TemplateArgs, ...)             \
    LUMINA_PRIVATE_TEMPLATE_STATIC_IMPL(LUMINA_PA_CAT(MemberName, _LmPrivTag_, __COUNTER__), \
        ClassType, MemberName, TemplateArgs, __VA_ARGS__)
#define LUMINA_PRIVATE_TEMPLATE_STATIC_IMPL(Tag, ClassType, MemberName, TemplateArgs, ...)   \
    namespace PrivateAccess                                                                  \
    {                                                                                        \
        struct Tag                                                                           \
        {                                                                                    \
            using TFn  = __VA_ARGS__;                                                        \
            using TPtr = TFn*;                                                               \
            friend TPtr GetMemberPtr(Tag);                                                   \
        };                                                                                   \
        template struct ::Lumina::PrivateAccess::TTagInstaller<Tag, Tag::TPtr,               \
            &ClassType::MemberName<LUMINA_PA_ESC TemplateArgs>>;                             \
        template<typename TClass, typename... TArgs>                                         \
        requires (std::is_same_v<TClass, ClassType>                                          \
                   && std::is_invocable_v<Tag::TPtr, TArgs...>)                              \
        constexpr decltype(auto) MemberName(TArgs&&... Args)                                 \
        {                                                                                    \
            return std::invoke(GetMemberPtr(Tag{}), std::forward<TArgs>(Args)...);           \
        }                                                                                    \
    }
