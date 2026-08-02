#pragma once

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"
#include "Core/LuminaMacros.h"

// Category-attributed allocation tracking for the Memory Profiler. Compiled out in Shipping; in
// Debug/Dev gated behind a runtime atomic (off by default) = one predicted branch per alloc until enabled.
#if defined(LE_SHIPPING)
    #define LUMINA_MEMORY_TRACKING 0
#else
    #define LUMINA_MEMORY_TRACKING 1
#endif

namespace Lumina::Memory
{
    // Categories are plain names (e.g. "Render Scene"), interned to a stable id on first use;
    // adding one is just a new string at a LUMINA_MEMORY_SCOPE, no central enum.
    struct FMemoryCategoryStats
    {
        const char* Name        = "";
        uint64      LiveBytes    = 0;   // currently outstanding bytes
        uint64      LiveCount    = 0;   // currently outstanding allocations
        uint64      PeakBytes    = 0;   // high-water mark of LiveBytes since reset
        uint64      TotalAllocs  = 0;   // cumulative allocations since reset
        uint64      TotalFrees   = 0;   // cumulative frees since reset
    };

    static constexpr int kMaxStackFrames = 24;

    // An aggregated allocation call site (one unique captured stack).
    struct FCallSiteStat
    {
        const char* CatName     = "";
        uint64      LiveBytes   = 0;
        uint64      LiveCount   = 0;
        uint64      TotalAllocs = 0;
        uint32      FrameCount  = 0;
        void*       Frames[kMaxStackFrames] = {};
    };

    // How GetTopCallSites ranks sites: by currently-live bytes (leaks / persistent), or by
    // cumulative allocation count (transient churn -- the per-frame allocations to eliminate).
    enum class ECallSiteSort : uint8
    {
        LiveBytes,
        TotalAllocs,
    };

#if LUMINA_MEMORY_TRACKING

    // Interns a category name and returns a stable id (idempotent, thread-safe, cached per call site).
    // Name must outlive the process (a string literal); it is copied into the registry.
    RUNTIME_API uint32 RegisterCategory(const char* Name);

    // Master switch. Cheap relaxed atomic; the alloc hooks early-out when false.
    RUNTIME_API void SetTrackingEnabled(bool bEnabled);
    RUNTIME_API bool IsTrackingEnabled();

    // Zeroes all category/call-site counters. Keeps the registered categories
    // (their ids are cached at call sites), so it just re-baselines the numbers.
    RUNTIME_API void ResetTracking();

    // Fills Out with per-category stats, returns the number written (<= MaxOut).
    RUNTIME_API uint32 GetCategoryStats(FMemoryCategoryStats* Out, uint32 MaxOut);

    RUNTIME_API uint64 GetTrackedLiveBytes();
    RUNTIME_API uint64 GetTrackedLiveCount();

    // Allocations that could not be recorded because a table hit its ceiling.
    // Non-zero means reported live bytes under-count.
    RUNTIME_API uint64 GetTrackingOverflowCount();

    // Per-allocation call-stack capture; heavier (walks the stack each alloc), so a separate opt-in.
    // Lets the profiler attribute live bytes to the exact leaking call site.
    RUNTIME_API void SetCaptureCallstacks(bool bEnabled);
    RUNTIME_API bool IsCapturingCallstacks();

    // Copies up to MaxOut call sites into Out, ranked descending by the chosen key.
    // Only sites with a non-zero key are returned. Returns the number written.
    RUNTIME_API uint32 GetTopCallSites(FCallSiteStat* Out, uint32 MaxOut, ECallSiteSort Sort = ECallSiteSort::LiveBytes);

    // Resolves a captured return address to "function (file:line)" text. Returns
    // false if symbols are unavailable (writes a hex address fallback in that case).
    RUNTIME_API bool ResolveSymbol(void* Address, char* OutText, size_t OutTextSize);

    namespace Hooks
    {
        // Called by Memory::Malloc/Realloc/Free. Cheap no-ops while disabled.
        RUNTIME_API void OnAlloc(void* Ptr, size_t Size);
        RUNTIME_API void OnFree(void* Ptr);
        RUNTIME_API void OnRealloc(void* OldPtr, void* NewPtr, size_t NewSize);

        RUNTIME_API void PushCategory(uint32 CategoryId);
        RUNTIME_API void PopCategory();
    }

    // RAII scope: every allocation made on this thread while the scope is live is
    // attributed to the named category (nesting is honored; innermost wins).
    struct FMemoryScope
    {
        FORCEINLINE explicit FMemoryScope(uint32 CategoryId) { Hooks::PushCategory(CategoryId); }
        FORCEINLINE ~FMemoryScope() { Hooks::PopCategory(); }
        LE_NO_COPYMOVE(FMemoryScope);
    };

    // Interns the name once per call site (function-local static), then scopes it.
    #define LUMINA_MEMORY_SCOPE(Name)                                                       \
        static const uint32 CAT(LumMemCatId_, __LINE__) = ::Lumina::Memory::RegisterCategory(Name); \
        ::Lumina::Memory::FMemoryScope CAT(LumMemScope_, __LINE__)(CAT(LumMemCatId_, __LINE__))

#else // Shipping: everything compiles to nothing.

    struct FMemoryScope
    {
        FORCEINLINE explicit FMemoryScope(uint32) {}
        LE_NO_COPYMOVE(FMemoryScope);
    };

    #define LUMINA_MEMORY_SCOPE(Name) ((void)0)

#endif
}
