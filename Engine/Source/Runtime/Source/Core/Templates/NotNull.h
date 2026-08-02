#pragma once

#include "Core/Assertions/Assert.h"
#include <concepts>

namespace Lumina
{
    template<typename T>
    concept NullComparable = requires(T p) 
    {
        { p != nullptr } -> std::convertible_to<bool>;
    };

    template<NullComparable T>
    class TNotNull
    {
    public:
        template<typename U>
        requires (!std::same_as<std::remove_cvref_t<U>, TNotNull> && std::convertible_to<U, T>)
        constexpr TNotNull(U&& Inst)
            : Ptr(eastl::forward<U>(Inst))
        {
            ASSERT(Ptr != nullptr);
        }

        TNotNull(const TNotNull&)            = default;
        TNotNull(TNotNull&&)                 = default;
        TNotNull& operator=(const TNotNull&) = default;
        TNotNull& operator=(TNotNull&&)      = default;
        ~TNotNull()                          = default;

        constexpr TNotNull(std::nullptr_t) = delete;
        TNotNull& operator=(std::nullptr_t) = delete;

        constexpr T Get() const { ASSERT(Ptr); return Ptr; }
        constexpr operator T() const { return Get(); }
        constexpr decltype(auto) operator->() const { return Get(); }
        constexpr decltype(auto) operator*()  const { return *Get(); }

    private:
        T Ptr;
    };
}
