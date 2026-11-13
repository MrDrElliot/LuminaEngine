#include "Core/Threading/Thread.h"
#include "RuntimePCH.h"
#include "Memory.h"
#include "DebugAllocator.h"
#include "Core/Threading/Atomic.h"
#include "MemoryTracking.h"
#include "Allocators/Allocator.h"
#include "Core/LuminaMacros.h"
#include "Core/Assertions/Assert.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Profiler/Profile.h"
#include "Core/Templates/Align.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"

namespace Lumina
{
    static void RPMallocAssert(const char* pMessage)
    {
        if (pMessage && strstr(pMessage, "Memory leak detected"))
        {
            return;
        }
        
        PANIC("{}", pMessage);
    }
    

    namespace
    {
        // Constant initialized so allocations made before the console variable registers read a valid depth.
        constinit TAtomic<int32> GAllocCallstackDepth{0};

        TConsoleVar<int32> CVarAllocCallstackDepth("Profiler.AllocCallstackDepth", 0,
            "Frames Tracy walks per allocation while connected. Each walk costs about a microsecond, which inflates allocating zones.",
            [](const CVarValueType& Value) { GAllocCallstackDepth.store(Containers::Get<int32>(Value), Atomic::MemoryOrderRelaxed); });
    }

    Memory::FMalloc::FMalloc() noexcept
    {
        rpmalloc_config_t Config = {};
        Config.error_callback = RPMallocAssert;
        rpmalloc_initialize_config(&Config);
    }

    namespace
    {
        Memory::FMalloc& EnsureAllocator()
        {
            static Memory::FMalloc Instance;
            return Instance;
        }
    }
    
    // Idempotent, so paying it on every malloc primitive is cheap even in monolithic Shipping.
    FORCEINLINE static void EnsureThisThreadInitialized()
    {
        rpmalloc_thread_initialize();
    }

    void* Memory::FMalloc::Malloc(size_t Size, size_t Alignment)
    {
        DEBUG_ASSERT(Alignment >= sizeof(void*));
        DEBUG_ASSERT((Alignment & (Alignment - 1)) == 0);
        DEBUG_ASSERT(Alignment % sizeof(void*) == 0);
        
        EnsureThisThreadInitialized();
    #if LUMINA_DEBUG_ALLOCATOR
        return DebugAllocator::Allocate(Size, Alignment);
    #else
        return rpaligned_alloc(Alignment, Size);
    #endif
    }

    void* Memory::FMalloc::Realloc(void* Memory, size_t NewSize, size_t Alignment)
    {
        DEBUG_ASSERT(Alignment >= sizeof(void*));
        DEBUG_ASSERT((Alignment & (Alignment - 1)) == 0);
        DEBUG_ASSERT(Alignment % sizeof(void*) == 0);
        EnsureThisThreadInitialized();
    #if LUMINA_DEBUG_ALLOCATOR
        return DebugAllocator::Reallocate(Memory, NewSize, Alignment);
    #else
        return rpaligned_realloc(Memory, Alignment, NewSize, 0, 0);
    #endif
    }

    void Memory::FMalloc::Free(void* Memory)
    {
        EnsureThisThreadInitialized();
    #if LUMINA_DEBUG_ALLOCATOR
        DebugAllocator::Free(Memory);
    #else
        rpfree(Memory);
    #endif
    }

#if LUMINA_DEBUG_ALLOCATOR
    size_t Memory::GetCurrentMappedMemory()    { return DebugAllocator::GetStats().LiveBytes; }
    size_t Memory::GetPeakMappedMemory()       { return DebugAllocator::GetStats().PeakLiveBytes; }
    size_t Memory::GetCachedMemory()           { return DebugAllocator::GetStats().QuarantineBytes; }
    size_t Memory::GetCurrentHugeAllocMemory() { return 0; }
    size_t Memory::GetPeakHugeAllocMemory()    { return 0; }
    size_t Memory::GetTotalMappedMemory()      { return 0; }
    size_t Memory::GetTotalUnmappedMemory()    { return 0; }
    size_t Memory::GetCommittedMemory()        { const DebugAllocator::FStats Stats = DebugAllocator::GetStats(); return Stats.LiveBytes + Stats.QuarantineBytes; }
    bool   Memory::UsesHugePages()             { return false; }

    bool Memory::DumpAllocatorStatistics(const char* Path)
    {
        return DebugAllocator::DumpLiveAllocations(Path);
    }
#else
    size_t Memory::GetCurrentMappedMemory()    { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.mapped; }
    size_t Memory::GetPeakMappedMemory()       { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.mapped_peak; }
    size_t Memory::GetCachedMemory()           { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.cached; }
    size_t Memory::GetCurrentHugeAllocMemory() { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.huge_alloc; }
    size_t Memory::GetPeakHugeAllocMemory()    { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.huge_alloc_peak; }
    size_t Memory::GetTotalMappedMemory()      { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.mapped_total; }
    size_t Memory::GetTotalUnmappedMemory()    { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.unmapped_total; }
    size_t Memory::GetCommittedMemory()        { return rpmalloc_committed_bytes(); }
    bool   Memory::UsesHugePages()             { return rpmalloc_uses_huge_pages() != 0; }

    bool Memory::DumpAllocatorStatistics(const char* Path)
    {
        FILE* File = std::fopen(Path, "w");
        if (File == nullptr)
        {
            return false;
        }
        rpmalloc_dump_statistics(File);
        fclose(File);
        return true;
    }
#endif

    namespace
    {
        struct FThreadCacheTotals
        {
            size_t Sum = 0;
            size_t Max = 0;
        };

        // The statistics dump is the only view rpmalloc gives of every thread's span cache, one Cached column per heap.
        FThreadCacheTotals SumThreadSpanCaches(const char* DumpPath)
        {
            FThreadCacheTotals Totals;
            FILE* File = std::fopen(DumpPath, "r");
            if (File == nullptr)
            {
                return Totals;
            }

            const size_t SpanSize = rpmalloc_config()->span_size;
            size_t HeapBytes = 0;
            bool bInSpanTable = false;
            char Line[512];
            while (std::fgets(Line, sizeof(Line), File) != nullptr)
            {
                uint32 SpanCount, Peak, Deferred, Cached;
                int32 Current;
                size_t PeakMiB;
                if (std::strncmp(Line, "Spans ", 6) == 0)
                {
                    bInSpanTable = true;
                }
                else if (bInSpanTable && std::sscanf(Line, "%u: %d %u %u %zu %u", &SpanCount, &Current, &Peak, &Deferred, &PeakMiB, &Cached) == 6)
                {
                    HeapBytes += size_t(Cached) * SpanCount * SpanSize;
                }
                else if (bInSpanTable)
                {
                    Totals.Sum += HeapBytes;
                    Totals.Max = Math::Max(Totals.Max, HeapBytes);
                    HeapBytes = 0;
                    bInSpanTable = false;
                }
            }
            fclose(File);
            return Totals;
        }

        FAutoConsoleCommand GAllocatorStatsCommand("Memory.AllocatorStats",
            "Logs allocator mapped, committed, live and cached bytes in one line and writes the full dump to Saved/AllocatorStats.txt.", []
            {
                const FString Directory = Paths::GetEngineDirectory() + "/Saved";
                Paths::CreateDirectories(FStringView(*Directory, Directory.Len()));
                const FString DumpPath = Directory + "/AllocatorStats.txt";
                const bool bDumped = Memory::DumpAllocatorStatistics(*DumpPath);
                const FThreadCacheTotals ThreadCaches = SumThreadSpanCaches(*DumpPath);

                const size_t Committed   = Memory::GetCommittedMemory();
                const size_t GlobalCache = Memory::GetCachedMemory();
                const size_t Cached      = GlobalCache + ThreadCaches.Sum;
                LOG_INFO("Allocator: mapped {} MB, committed {} MB, OS commit {} MB, live {} MB, global cache {} MB, thread span cache {} MB (largest {} MB), huge {} MB, huge pages {}. Dump {}.",
                    Memory::GetCurrentMappedMemory() >> 20, Committed >> 20, Platform::GetProcessMemoryUsageBytes() >> 20,
                    (Committed > Cached ? Committed - Cached : 0) >> 20, GlobalCache >> 20, ThreadCaches.Sum >> 20, ThreadCaches.Max >> 20,
                    Memory::GetCurrentHugeAllocMemory() >> 20, Memory::UsesHugePages() ? "on" : "off", bDumped ? DumpPath : FString("failed"));
            });
    }

    namespace
    {
        std::atomic<uint32> GCacheTrimEpoch{0};
        thread_local uint32 GThreadTrimEpoch = 0;
    }

    void Memory::RequestCacheTrim()
    {
        GCacheTrimEpoch.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("Allocator cache trim requested; job workers release their caches and the shared span cache when they next go idle.");
    }

    void Memory::TrimThreadCacheIfRequested(bool bForce)
    {
        const uint32 Epoch = GCacheTrimEpoch.load(std::memory_order_relaxed);
        if (Epoch == GThreadTrimEpoch && !bForce)
        {
            return;
        }
        LUMINA_PROFILE_SCOPE();
        GThreadTrimEpoch = Epoch;
        rpmalloc_thread_trim_caches(SIZE_MAX);
        rpmalloc_global_trim_caches(SIZE_MAX);
    }

    size_t Memory::GetThreadCachedSpans()
    {
        rpmalloc_thread_statistics_t Stats{};
        rpmalloc_thread_statistics(&Stats);
        return Stats.spancache / rpmalloc_config()->span_size;
    }

    size_t Memory::TrimThreadCache(size_t MaxSpans)
    {
        LUMINA_PROFILE_SCOPE();
        return rpmalloc_thread_trim_caches(MaxSpans);
    }

    void Memory::Initialize()
    {
        (void)EnsureAllocator();
    }

    void Memory::InitializeThreadHeap()
    {
        (void)EnsureAllocator();
        rpmalloc_thread_initialize();
    }

    // Per-thread rpmalloc init happens inside FMalloc's primitives; the public wrappers needn't repeat it.
    void* Memory::Malloc(size_t Size, size_t Alignment)
    {
        FMalloc& Allocator = EnsureAllocator();
        void* pMemory = Allocator.Malloc(Size, Align<size_t>(Alignment, 16));
        TracyCAllocS(pMemory, Size, GAllocCallstackDepth.load(Atomic::MemoryOrderRelaxed));
    #if LUMINA_MEMORY_TRACKING
        ::Lumina::Memory::Hooks::OnAlloc(pMemory, Size);
    #endif
        return pMemory;
    }

    void* Memory::MallocUntracked(size_t Size, size_t Alignment)
    {
        FMalloc& Allocator = EnsureAllocator();
        void* pMemory = Allocator.Malloc(Size, Align<size_t>(Alignment, 16));
        TracyCAllocS(pMemory, Size, GAllocCallstackDepth.load(Atomic::MemoryOrderRelaxed));
        return pMemory;
    }

    void Memory::FreeUntracked(void* Memory)
    {
        TracyCFreeS(Memory, GAllocCallstackDepth.load(Atomic::MemoryOrderRelaxed));
        FMalloc& Allocator = EnsureAllocator();
        Allocator.Free(Memory);
    }

    void* Memory::Realloc(void* Memory, size_t NewSize, size_t Alignment)
    {
        const int32 CallstackDepth = GAllocCallstackDepth.load(Atomic::MemoryOrderRelaxed);
        if (Memory != nullptr)
        {
            TracyCFreeS(Memory, CallstackDepth);
        }
        FMalloc& Allocator = EnsureAllocator();
        void* pMemory = Allocator.Realloc(Memory, NewSize, Align<size_t>(Alignment, 16));
        if (pMemory != nullptr)
        {
            TracyCAllocS(pMemory, NewSize, CallstackDepth);
        }
    #if LUMINA_MEMORY_TRACKING
        ::Lumina::Memory::Hooks::OnRealloc(Memory, pMemory, NewSize);
    #endif
        return pMemory;
    }

    size_t Memory::GetAllocationSize(void* Memory)
    {
    #if LUMINA_DEBUG_ALLOCATOR
        return DebugAllocator::GetAllocationSize(Memory);
    #else
        return (Memory != nullptr) ? rpmalloc_usable_size(Memory) : 0;
    #endif
    }

    bool Memory::TryExpandInPlace(void* Memory, size_t NewSize)
    {
        if (Memory == nullptr || GetAllocationSize(Memory) < NewSize)
        {
            return false;
        }

        const int32 CallstackDepth = GAllocCallstackDepth.load(Atomic::MemoryOrderRelaxed);
        TracyCFreeS(Memory, CallstackDepth);
        TracyCAllocS(Memory, NewSize, CallstackDepth);
    #if LUMINA_MEMORY_TRACKING
        ::Lumina::Memory::Hooks::OnRealloc(Memory, Memory, NewSize);
    #endif
        return true;
    }

    void Memory::Free(void*& Memory)
    {
        TracyCFreeS(Memory, GAllocCallstackDepth.load(Atomic::MemoryOrderRelaxed));
    #if LUMINA_MEMORY_TRACKING
        ::Lumina::Memory::Hooks::OnFree(Memory);
    #endif
        FMalloc& Allocator = EnsureAllocator();
        Allocator.Free(Memory);
        Memory = nullptr;
    }

    FBlockLinearAllocator& GetThreadScratchAllocator()
    {
        // 256 KB blocks; chains more on demand and reuses them across FMemMark scopes.
        static constexpr SIZE_T ScratchBlockSize = 256 * 1024;
        thread_local FBlockLinearAllocator GScratch(ScratchBlockSize);
        return GScratch;
    }

    namespace Memory
    {
        void* ScratchAllocate(size_t Size, size_t Alignment)
        {
            return GetThreadScratchAllocator().Allocate(Size, Alignment);
        }

        void* FrameAllocate(size_t Size, size_t Alignment)
        {
            return GetThreadFrameAllocator().Allocate(Size, Alignment);
        }
    }

    namespace
    {
        // Register and unregister happen once per thread and ResetAll once per frame, so a mutex is plenty.
        struct FFrameArenaNode
        {
            FBlockLinearAllocator* Arena = nullptr;
            FFrameArenaNode*       Next  = nullptr;

            // Largest frame in the current window, so blocks only a load spike needed are handed back.
            SIZE_T                 WindowPeakBlocks = 0;
            uint32                 WindowFrames     = 0;
        };

        // About ten seconds at 60 Hz, long enough that a periodic burst keeps its blocks.
        constexpr uint32 kFrameArenaTrimWindow = 600;

        FMutex       GFrameArenaMutex;
        FFrameArenaNode* GFrameArenaHead = nullptr;

        // The thread_local owner constructs the arena, links it in, and unlinks on thread exit.
        struct FThreadFrameArena
        {
            // A single container growth must fit one block, and chains beyond it on demand.
            static constexpr SIZE_T FrameBlockSize = 8 * Constants::kMiB;

            FBlockLinearAllocator Allocator{ FrameBlockSize };
            FFrameArenaNode       Node;

            FThreadFrameArena()
            {
                Node.Arena = &Allocator;
                FScopeLock Lock(GFrameArenaMutex);
                Node.Next       = GFrameArenaHead;
                GFrameArenaHead = &Node;
            }

            ~FThreadFrameArena()
            {
                FScopeLock Lock(GFrameArenaMutex);
                FFrameArenaNode** Link = &GFrameArenaHead;
                while (*Link && *Link != &Node)
                {
                    Link = &(*Link)->Next;
                }
                if (*Link == &Node)
                {
                    *Link = Node.Next;
                }
            }
            FThreadFrameArena(const FThreadFrameArena&) = delete;
            FThreadFrameArena& operator=(const FThreadFrameArena&) = delete;
            FThreadFrameArena(FThreadFrameArena&&) = delete;
            FThreadFrameArena& operator=(FThreadFrameArena&&) = delete;
        };
    }

    FBlockLinearAllocator& GetThreadFrameAllocator()
    {
        thread_local FThreadFrameArena GFrameArena;
        return GFrameArena.Allocator;
    }

    void ResetThreadFrameAllocators()
    {
        FScopeLock Lock(GFrameArenaMutex);
        for (FFrameArenaNode* N = GFrameArenaHead; N != nullptr; N = N->Next)
        {
            N->WindowPeakBlocks = Math::Max(N->WindowPeakBlocks, N->Arena->GetBlocksInUse());
            N->Arena->Reset();

            if (++N->WindowFrames >= kFrameArenaTrimWindow)
            {
                if (N->Arena->GetBlockCount() > N->WindowPeakBlocks)
                {
                    N->Arena->TrimBlocks(N->WindowPeakBlocks);
                }
                N->WindowPeakBlocks = 0;
                N->WindowFrames     = 0;
            }
        }
    }
}

#if defined(LE_PLATFORM_WINDOWS)
    #pragma comment(linker, "/EXPORT:LmThirdPartyMalloc")
    #pragma comment(linker, "/EXPORT:LmThirdPartyRealloc")
    #pragma comment(linker, "/EXPORT:LmThirdPartyCalloc")
    #pragma comment(linker, "/EXPORT:LmThirdPartyFree")
#endif

#if LUMINA_MEMORY_TRACKING
    #define LM_TP_SCOPE(Category) \
        ::Lumina::Memory::FMemoryScope LmTpScope(::Lumina::Memory::RegisterCategory((Category) ? (Category) : "ThirdParty"))
#else
    #define LM_TP_SCOPE(Category) ((void)(Category))
#endif

extern "C" void* LmThirdPartyMalloc(size_t Size, const char* Category)
{
    LM_TP_SCOPE(Category);
    return ::Lumina::Memory::Malloc(Size);
}

extern "C" void* LmThirdPartyRealloc(void* Ptr, size_t Size, const char* Category)
{
    LM_TP_SCOPE(Category);
    return ::Lumina::Memory::Realloc(Ptr, Size);
}

extern "C" void* LmThirdPartyCalloc(size_t Count, size_t Size, const char* Category)
{
    const size_t Total = Count * Size;
    if (Count != 0 && Total / Count != Size) // multiply overflow
    {
        return nullptr;
    }

    LM_TP_SCOPE(Category);
    void* Ptr = ::Lumina::Memory::Malloc(Total);
    if (Ptr != nullptr)
    {
        ::Lumina::Memory::Memset(Ptr, 0, Total);
    }
    return Ptr;
}

extern "C" void LmThirdPartyFree(void* Ptr)
{
    if (Ptr != nullptr)
    {
        ::Lumina::Memory::Free(Ptr);
    }
}

#undef LM_TP_SCOPE
