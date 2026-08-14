#pragma once

#if defined(LE_DEBUG)
#define ENABLE_VALIDATE_ARGS
#endif
#include <rpmalloc.h>
#include <utility>
#include <xmmintrin.h>
#include <EASTL/type_traits.h>

#include "Core/LuminaMacros.h"
#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"
#include "tracy/TracyC.h"


#define LUMINA_PROFILE_ALLOC(p, size)   TracyCAllocS(p, size, 12)
#define LUMINA_PROFILE_FREE(p)          TracyCFreeS(p, 12)

constexpr size_t DEFAULT_ALIGNMENT = 16;

namespace Lumina::Memory
{
    struct FMalloc
    {
        FMalloc() noexcept;
        ~FMalloc() = default;
        LE_NO_COPYMOVE(FMalloc);

        void* Malloc(size_t Size, size_t Alignment);
        void* Realloc(void* Memory, size_t NewSize, size_t Alignment);
        void Free(void* Memory);
    };
    
    RUNTIME_API extern FMalloc* GMalloc;
    
    
    inline void Memzero(void* Ptr, size_t Size)
    {
        std::memset(Ptr, 0, Size);
    }
    
    template <typename T>
    void Memzero(T* Ptr)
    {
        std::memset(Ptr, 0, sizeof(T));
    }

    inline void Memset(void* Ptr, int Val, size_t Size)
    {
        std::memset(Ptr, Val, Size);
    }

    NODISCARD inline bool MemEqual(const void* A, const void* B, size_t Size)
    {
        return std::memcmp(A, B, Size) == 0;
    }

    template <typename T>
    NODISCARD bool MemEqual(const T* A, const T* B)
    {
        return std::memcmp(A, B, sizeof(T)) == 0;
    }

    // Hint the cache to pull the line containing Ptr into every level. A pure scheduling hint:
    // safe on any address (never faults), never required for correctness.
    FORCEINLINE void Prefetch(const void* Ptr)
    {
#if defined(_MSC_VER) || defined(__SSE__)
        _mm_prefetch(static_cast<const char*>(Ptr), _MM_HINT_T0);
#else
        __builtin_prefetch(Ptr);
#endif
    }

    // Prefetch every cache line of [Ptr, Ptr + Size); Size is typically sizeof(*Ptr).
    FORCEINLINE void Prefetch(const void* Ptr, size_t Size)
    {
        const char* Base = static_cast<const char*>(Ptr);
        for (size_t Offset = 0; Offset < Size; Offset += 64)
        {
            Prefetch(Base + Offset);
        }
    }

    RUNTIME_API void Initialize();
    
    RUNTIME_API void InitializeThreadHeap();

    NODISCARD inline bool IsThreadHeapInitialized()
    {
        return rpmalloc_is_thread_initialized();
    }

    inline void ShutdownThreadHeap()
    {
        rpmalloc_thread_finalize(1);
    }

    // rpmalloc global stats; MUST be exported (each module links its own rpmalloc, an inline body
    // would query the caller's empty instance). Return 0 unless built with ENABLE_STATISTICS.
    RUNTIME_API NODISCARD size_t GetCurrentMappedMemory();
    RUNTIME_API NODISCARD size_t GetPeakMappedMemory();
    RUNTIME_API NODISCARD size_t GetCachedMemory();
    RUNTIME_API NODISCARD size_t GetCurrentHugeAllocMemory();
    RUNTIME_API NODISCARD size_t GetPeakHugeAllocMemory();
    RUNTIME_API NODISCARD size_t GetTotalMappedMemory();
    RUNTIME_API NODISCARD size_t GetTotalUnmappedMemory();
    
    RUNTIME_API NODISCARD void* Malloc(size_t Size, size_t Alignment = DEFAULT_ALIGNMENT);
    
    RUNTIME_API NODISCARD void* Realloc(void* Memory, size_t NewSize, size_t OriginalAlignment = DEFAULT_ALIGNMENT);

    RUNTIME_API void Free(void*& Memory);

    template<typename T, typename ... ConstructorParams>
    requires eastl::is_constructible_v<T, ConstructorParams...> && (!eastl::is_array_v<T>)
    NODISCARD FORCEINLINE T* New(ConstructorParams&&... Params)  // NOLINT(cppcoreguidelines-missing-std-forward)
    {
        void* Memory = Malloc(sizeof(T), alignof(T));
        return new(Memory) T(eastl::forward<ConstructorParams>(Params)...);
    }

    
    template<typename T, typename ... TArgs>
    NODISCARD FORCEINLINE T* NewArray(const size_t NumElements, TArgs&&... Args)
    {
        const size_t RequiredAlignment = Math::Max(alignof(T), size_t(16));
        const size_t RequiredExtraMemory = Math::Max(RequiredAlignment, size_t(4));
        const size_t RequiredMemory = sizeof(T) * NumElements + RequiredExtraMemory;

        uint8* pOriginalAddress = (uint8*) Malloc(RequiredMemory, RequiredAlignment);

        T* pArrayAddress = reinterpret_cast<T*>(pOriginalAddress + RequiredExtraMemory);
        for (size_t i = 0; i < NumElements; i++)
        {
            new(&pArrayAddress[i]) T(std::forward<TArgs>(Args)...);
        }

        uint32* pNumElements = reinterpret_cast<uint32_t*>( pArrayAddress ) - 1;
        *pNumElements = uint32(NumElements);

        return reinterpret_cast<T*>( pArrayAddress );
    }

    template<typename T>
    NODISCARD FORCEINLINE T* NewArray(const size_t NumElements, const T& Value)
    {
        const size_t RequiredAlignment = Math::Max(alignof(T), size_t(16));
        const size_t RequiredExtraMemory = Math::Max(RequiredAlignment, size_t(4));
        const size_t RequiredMemory = sizeof(T) * NumElements + RequiredExtraMemory;

        uint8* pOriginalAddress = (uint8*) Malloc(RequiredMemory, RequiredAlignment);

        T* pArrayAddress = reinterpret_cast<T*>(pOriginalAddress + RequiredExtraMemory);
        for (size_t i = 0; i < NumElements; i++)
        {
            new(&pArrayAddress[i]) T(Value);
        }

        uint32* pNumElements = reinterpret_cast<uint32_t*>( pArrayAddress ) - 1;
        *pNumElements = uint32(NumElements);

        return pArrayAddress;
    }

    template<typename T>
    FORCEINLINE void DeleteArray(T* Array)
    {
        const size_t RequiredAlignment = std::max(alignof(T), static_cast<size_t>(16));
        const size_t RequiredExtraMemory = std::max(RequiredAlignment, static_cast<size_t>(4));

        const uint32 NumElements = *(reinterpret_cast<uint32*>(Array) - 1);
        
        if (!eastl::is_trivially_destructible_v<T>)
        {
            for (uint32 i = 0; i < NumElements; i++)
            {
                Array[i].~T();
            }
        }

        uint8* OriginalAddress = reinterpret_cast<uint8*>(Array) - RequiredExtraMemory;
        Free((void*&)OriginalAddress);
    }
    
    template<typename T>
    void Delete(T* Type)
    {
        if constexpr (!eastl::is_trivially_destructible_v<T>)
        {
            Type->~T();
        }
        
        Free((void*&)Type);
    }

    template< typename T >
    void Free(T*& Type)
    {
        Free((void*&)Type);
    }
}

#if defined(LE_PLATFORM_WINDOWS) || defined(REFLECTION_PARSER)
    #define LUMINA_THIRDPARTY_ALLOC_API
#else
    #define LUMINA_THIRDPARTY_ALLOC_API DLL_EXPORT
#endif

extern "C"
{
    LUMINA_THIRDPARTY_ALLOC_API void* LmThirdPartyMalloc(size_t Size, const char* Category);
    LUMINA_THIRDPARTY_ALLOC_API void* LmThirdPartyRealloc(void* Ptr, size_t Size, const char* Category);
    LUMINA_THIRDPARTY_ALLOC_API void* LmThirdPartyCalloc(size_t Count, size_t Size, const char* Category);
    LUMINA_THIRDPARTY_ALLOC_API void  LmThirdPartyFree(void* Ptr);
}


#define DECLARE_MODULE_ALLOCATOR_OVERRIDES() \
    void* operator new(std::size_t size) { return Lumina::Memory::Malloc(size); } \
    void operator delete(void* ptr) noexcept { Lumina::Memory::Free(ptr); } \
    void* operator new[](std::size_t size) { return Lumina::Memory::Malloc(size); } \
    void operator delete[](void* ptr) noexcept { Lumina::Memory::Free(ptr); } \
    void* operator new(std::size_t size, std::align_val_t align) { return Lumina::Memory::Malloc(size, static_cast<size_t>(align)); } \
    void* operator new[](std::size_t size, std::align_val_t align) { return Lumina::Memory::Malloc(size, static_cast<size_t>(align)); } \
    void operator delete(void* ptr, std::align_val_t) noexcept { Lumina::Memory::Free(ptr); } \
    void operator delete[](void* ptr, std::align_val_t) noexcept { Lumina::Memory::Free(ptr); } \
    void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return Lumina::Memory::Malloc(size); } \
    void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return Lumina::Memory::Malloc(size); } \
    void operator delete(void* ptr, const std::nothrow_t&) noexcept { Lumina::Memory::Free(ptr); } \
    void operator delete[](void* ptr, const std::nothrow_t&) noexcept { Lumina::Memory::Free(ptr); } \
    void operator delete(void* ptr, std::size_t) noexcept { Lumina::Memory::Free(ptr); } \
    void operator delete[](void* ptr, std::size_t) noexcept { Lumina::Memory::Free(ptr); } \
    void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { Lumina::Memory::Free(ptr); } \
    void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { Lumina::Memory::Free(ptr); } \


