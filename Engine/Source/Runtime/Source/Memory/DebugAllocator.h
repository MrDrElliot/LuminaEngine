#pragma once

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

#ifndef LUMINA_DEBUG_ALLOCATOR
    #define LUMINA_DEBUG_ALLOCATOR 0
#endif

#if LUMINA_DEBUG_ALLOCATOR

// A slow, checking replacement for rpmalloc behind every Memory:: allocation, built only with -DebugAllocator=on.
namespace Lumina::Memory::DebugAllocator
{
    enum class EError : uint8
    {
        DoubleFree,
        FreedPointerUsed,
        InvalidPointer,
        HeaderCorrupted,
        Underrun,
        Overrun,
        UseAfterFree,
        InvalidArgument,
    };

    struct FError
    {
        EError              Kind = EError::InvalidPointer;
        const char*         Operation = "";
        const void*         Pointer = nullptr;
        uint64              Size = 0;
        uint64              Alignment = 0;
        uint64              Serial = 0;
        uint64              AllocThread = 0;
        uint64              FreeThread = 0;

        int64               Offset = 0;
        uint8               Expected = 0;
        uint8               Found = 0;

        void* const*        AllocStack = nullptr;
        uint32              AllocStackDepth = 0;
        void* const*        FreeStack = nullptr;
        uint32              FreeStackDepth = 0;
    };

    struct FStats
    {
        uint64              LiveBytes = 0;
        uint64              LiveCount = 0;
        uint64              PeakLiveBytes = 0;
        uint64              QuarantineBytes = 0;
        uint64              QuarantineCount = 0;
        uint64              TotalAllocations = 0;
        uint64              TotalFrees = 0;
        uint64              ErrorsReported = 0;
        bool                bPageGuard = false;
    };

    using FErrorHandler = void (*)(const FError& Error);

    RUNTIME_API void* Allocate(size_t Size, size_t Alignment);
    RUNTIME_API void* Reallocate(void* Memory, size_t NewSize, size_t Alignment);
    RUNTIME_API void Free(void* Memory);

    // Exactly what was asked for, so a caller using the slack another allocator would have given is an overrun here.
    NODISCARD RUNTIME_API size_t GetAllocationSize(const void* Memory);

    // Null restores the default, which reports through the crash handler and does not return.
    RUNTIME_API FErrorHandler SetErrorHandler(FErrorHandler Handler);

    RUNTIME_API uint64 ValidateHeap();

    RUNTIME_API void FlushQuarantine();

    NODISCARD RUNTIME_API FStats GetStats();
    NODISCARD RUNTIME_API const char* GetErrorName(EError Kind);

    RUNTIME_API bool DumpLiveAllocations(const char* Path);
}

#endif
