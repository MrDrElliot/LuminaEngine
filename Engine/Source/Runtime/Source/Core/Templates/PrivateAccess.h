#pragma once

#include "Containers/Invoke.h"
#include <type_traits>
#include <utility>

// Reaches a private member without modifying the class. Template arguments are parenthesized so signatures may contain commas.

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
            return ::Lumina::Invoke(GetMemberPtr(Tag{}), std::forward<TArgs>(Args)...);           \
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
                return ::Lumina::Invoke(GetMemberPtr(Tag{}), std::forward<TArgs>(Args)...);       \
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
            return ::Lumina::Invoke(GetMemberPtr(Tag{}), std::forward<TArgs>(Args)...);           \
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
            return ::Lumina::Invoke(GetMemberPtr(Tag{}), std::forward<TArgs>(Args)...);           \
        }                                                                                    \
    }
