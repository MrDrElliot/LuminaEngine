#pragma once

#include <type_traits>
#include <utility>

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina::Containers
{
    namespace Private
    {
        template <typename TClass, typename TMember, typename TObject, typename... TArgs>
        constexpr decltype(auto) InvokeMember(TMember TClass::* Member, TObject&& Object, TArgs&&... Args)
        {
            constexpr bool bHeldDirectly = std::is_base_of_v<TClass, std::remove_cvref_t<TObject>>;

            if constexpr (std::is_function_v<TMember>)
            {
                if constexpr (bHeldDirectly)
                {
                    return (std::forward<TObject>(Object).*Member)(std::forward<TArgs>(Args)...);
                }
                else
                {
                    return ((*std::forward<TObject>(Object)).*Member)(std::forward<TArgs>(Args)...);
                }
            }
            else
            {
                static_assert(sizeof...(TArgs) == 0, "A pointer to data member takes the object and nothing else.");

                if constexpr (bHeldDirectly)
                {
                    return std::forward<TObject>(Object).*Member;
                }
                else
                {
                    return (*std::forward<TObject>(Object)).*Member;
                }
            }
        }
    }

    /** Calls anything callable: a functor, a function pointer, a pointer to member function, a pointer to member data. */
    template <typename TCallable, typename... TArgs>
    constexpr decltype(auto) Invoke(TCallable&& Callable, TArgs&&... Args)
    {
        if constexpr (std::is_member_pointer_v<std::remove_cvref_t<TCallable>>)
        {
            return Private::InvokeMember(Callable, std::forward<TArgs>(Args)...);
        }
        else
        {
            return std::forward<TCallable>(Callable)(std::forward<TArgs>(Args)...);
        }
    }

    /** Invoke, converted to TRet; a void TRet discards whatever the target returns. */
    template <typename TRet, typename TCallable, typename... TArgs>
    constexpr TRet InvokeR(TCallable&& Callable, TArgs&&... Args)
    {
        if constexpr (std::is_void_v<TRet>)
        {
            Invoke(std::forward<TCallable>(Callable), std::forward<TArgs>(Args)...);
        }
        else
        {
            return Invoke(std::forward<TCallable>(Callable), std::forward<TArgs>(Args)...);
        }
    }

    template <typename TCallable, typename... TArgs>
    using TInvokeResult = std::invoke_result_t<TCallable, TArgs...>;
}

namespace Lumina
{
    using Containers::Invoke;
    using Containers::InvokeR;

    template <typename TCallable, typename... TArgs>
    using TInvokeResult = Containers::TInvokeResult<TCallable, TArgs...>;
}
