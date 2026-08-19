#pragma once

#include <cstring>
#include <new>
#include <utility>

#include "ContainerTraits.h"
#include "Memory/Memcpy.h"

namespace Lumina::ElementOps
{
    template <typename T>
    FORCEINLINE void DefaultConstructRange(T* Dest, size_t Count)
    {
        if constexpr (std::is_trivially_default_constructible_v<T>)
        {
            if (Count != 0)
            {
                Memory::Memzero(static_cast<void*>(Dest), Count * sizeof(T));
            }
        }
        else
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                ::new (static_cast<void*>(Dest + Index)) T();
            }
        }
    }

    template <typename T>
    FORCEINLINE void DestructRange(T* Dest, size_t Count)
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                Dest[Index].~T();
            }
        }
        else
        {
            (void)Dest;
            (void)Count;
        }
    }

    template <typename T>
    FORCEINLINE void CopyConstructRange(T* RESTRICT Dest, const T* RESTRICT Src, size_t Count)
    {
        if constexpr (std::is_trivially_copyable_v<T>)
        {
            if (Count != 0)
            {
                Memory::Memcpy(Dest, Src, Count * sizeof(T));
            }
        }
        else
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                ::new (static_cast<void*>(Dest + Index)) T(Src[Index]);
            }
        }
    }

    template <typename T>
    FORCEINLINE void MoveConstructRange(T* RESTRICT Dest, T* RESTRICT Src, size_t Count)
    {
        if constexpr (std::is_trivially_copyable_v<T>)
        {
            if (Count != 0)
            {
                Memory::Memcpy(Dest, Src, Count * sizeof(T));
            }
        }
        else
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                ::new (static_cast<void*>(Dest + Index)) T(std::move(Src[Index]));
            }
        }
    }

    /** Moves a range to fresh storage and ends the source objects; one memcpy when the type allows it. */
    template <typename T>
    FORCEINLINE void RelocateRange(T* RESTRICT Dest, T* RESTRICT Src, size_t Count)
    {
        if constexpr (TIsTriviallyRelocatable_V<T>)
        {
            if (Count != 0)
            {
                Memory::Memcpy(Dest, Src, Count * sizeof(T));
            }
        }
        else
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                ::new (static_cast<void*>(Dest + Index)) T(std::move(Src[Index]));
                Src[Index].~T();
            }
        }
    }

    /** RelocateRange where the ranges may overlap, as when opening or closing a gap for insert or erase. */
    template <typename T>
    FORCEINLINE void RelocateRangeOverlapping(T* Dest, T* Src, size_t Count)
    {
        if (Count == 0 || Dest == Src)
        {
            return;
        }

        if constexpr (TIsTriviallyRelocatable_V<T>)
        {
            std::memmove(static_cast<void*>(Dest), static_cast<const void*>(Src), Count * sizeof(T));
        }
        else if (Dest < Src)
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                ::new (static_cast<void*>(Dest + Index)) T(std::move(Src[Index]));
                Src[Index].~T();
            }
        }
        else
        {
            for (size_t Index = Count; Index-- > 0;)
            {
                ::new (static_cast<void*>(Dest + Index)) T(std::move(Src[Index]));
                Src[Index].~T();
            }
        }
    }

    template <typename T>
    FORCEINLINE void FillConstructRange(T* Dest, size_t Count, const T& Value)
    {
        for (size_t Index = 0; Index < Count; ++Index)
        {
            ::new (static_cast<void*>(Dest + Index)) T(Value);
        }
    }

    template <typename T>
    NODISCARD FORCEINLINE bool RangeEquals(const T* A, const T* B, size_t Count)
    {
        if constexpr (TIsBitwiseComparable_V<T>)
        {
            return Count == 0 || std::memcmp(A, B, Count * sizeof(T)) == 0;
        }
        else
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                if (!(A[Index] == B[Index]))
                {
                    return false;
                }
            }
            return true;
        }
    }
}
