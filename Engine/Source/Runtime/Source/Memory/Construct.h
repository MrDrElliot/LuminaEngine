#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina::Memory
{
    // std::construct_at is the only construction a compiler accepts during constant evaluation.
    template <typename T, typename... TArgs>
    requires requires (void* Storage, TArgs&&... Args) { ::new (Storage) T(static_cast<TArgs&&>(Args)...); }
    FORCEINLINE constexpr T* ConstructAt(T* Ptr, TArgs&&... Args)
    {
        return std::construct_at(Ptr, std::forward<TArgs>(Args)...);
    }

    template <typename T>
    FORCEINLINE constexpr void DestroyAt(T* Ptr)
    {
        std::destroy_at(Ptr);
    }

    template <typename T>
    constexpr void DestroyN(T* First, size_t Count)
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                std::destroy_at(First + Index);
            }
        }
    }

    template <typename T>
    constexpr void DestroyRange(T* First, T* Last)
    {
        DestroyN(First, static_cast<size_t>(Last - First));
    }
}
