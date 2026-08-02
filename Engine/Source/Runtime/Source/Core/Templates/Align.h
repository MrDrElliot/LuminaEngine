#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina
{
    #define LE_CACHELINE_SIZE (64)
    #define LE_COUNTOF(A) ::Lumina;:CountOf(A);
    #define LE_COUNTOF32(A) uint32(::Lumina::CountOf(A))
    #define LE_OFFSETOF(A) ::Lumina::OffsetOf(&A)
    #define LE_INCLUDES_MEMBER(S, A) ::Lumina::IncludesMember(S, &A)
    
    template<typename T, size_t N>
    constexpr size_t CountOf(const T (&)[N])
    {
        return N;
    }
    
    template<typename T, typename U>
    constexpr uint32 OffsetOf(U T::*Member)
    {
        return (uint32)((char*)&((T*)nullptr->*Member) - (char*)nullptr);
    }
    
    template<typename T, typename U>
    constexpr uint32 IncludesMember(size_t SizeOfStruct, U T::*Member)
    {
        return SizeOfStruct >= OffsetOf(Member) + sizeof(U);
    }
    
    template <typename T>
    constexpr T Align(T X, size_t Alignment)
    {
        return (T)(((size_t)X + Alignment - 1) / Alignment * Alignment);
    }
    
    template <typename T>
    T* Align(T* X, size_t Alignment)
    {
        return (T*)(((size_t)X + Alignment - 1) / Alignment * Alignment);
    }
    
    template <typename T>
    constexpr T AlignedSize(const T& Size, uint32 Alignment)
    {
        return ((Size + Alignment - 1) / Alignment) * Alignment;
    }
}
