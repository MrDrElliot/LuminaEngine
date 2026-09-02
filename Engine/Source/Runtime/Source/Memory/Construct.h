#pragma once

// Placement operator new is non-replaceable, so its declaration has to come from here.
#include <new>

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

// P2747 lets a plain placement new be constant-evaluated; without it std::construct_at is the only one.
#if defined(__cpp_constexpr) && __cpp_constexpr >= 202406L
    #define LUMINA_HAS_CONSTEXPR_PLACEMENT_NEW 1
#else
    #define LUMINA_HAS_CONSTEXPR_PLACEMENT_NEW 0
    #include <memory>
#endif

namespace Lumina::Memory
{
    template <typename T, typename... TArgs>
    requires requires (void* Storage, TArgs&&... Args) { ::new (Storage) T(static_cast<TArgs&&>(Args)...); }
    FORCEINLINE constexpr T* ConstructAt(T* Ptr, TArgs&&... Args)
    {
#if LUMINA_HAS_CONSTEXPR_PLACEMENT_NEW
        return ::new (static_cast<void*>(Ptr)) T(static_cast<TArgs&&>(Args)...);
#else
        return std::construct_at(Ptr, static_cast<TArgs&&>(Args)...);
#endif
    }

    template <typename T>
    FORCEINLINE constexpr void DestroyAt(T* Ptr)
    {
        Ptr->~T();
    }

    template <typename T>
    constexpr void DestroyN(T* First, SIZE_T Count)
    {
        if constexpr (!__is_trivially_destructible(T))
        {
            for (SIZE_T Index = 0; Index < Count; ++Index)
            {
                (First + Index)->~T();
            }
        }
    }

    template <typename T>
    constexpr void DestroyRange(T* First, T* Last)
    {
        DestroyN(First, static_cast<SIZE_T>(Last - First));
    }
}
