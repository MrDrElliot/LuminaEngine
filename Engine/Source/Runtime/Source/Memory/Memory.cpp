#include "Core/Threading/Thread.h"
#include "RuntimePCH.h"
#include "Memory.h"
#include "MemoryTracking.h"
#include "Allocators/Allocator.h"
#include "Core/LuminaMacros.h"
#include "Core/Assertions/Assert.h"
#include "Core/Profiler/Profile.h"
#include "Core/Templates/Align.h"

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
    
    RUNTIME_API Memory::FMalloc* Memory::GMalloc = nullptr;

    Memory::FMalloc::FMalloc() noexcept
    {
        rpmalloc_config_t Config = {};
        Config.error_callback = RPMallocAssert;
        Config.enable_huge_pages = true;
        rpmalloc_initialize_config(&Config);
    }

    namespace
    {
        // Magic-static bootstrap so dynamic-init allocations come up cleanly; also publishes GMalloc.
        Memory::FMalloc& EnsureAllocator()
        {
            static Memory::FMalloc Instance;
            Memory::GMalloc = &Instance;
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
        return rpaligned_alloc(Alignment, Size);
    }

    void* Memory::FMalloc::Realloc(void* Memory, size_t NewSize, size_t Alignment)
    {
        DEBUG_ASSERT(Alignment >= sizeof(void*));
        DEBUG_ASSERT((Alignment & (Alignment - 1)) == 0);
        DEBUG_ASSERT(Alignment % sizeof(void*) == 0);
        EnsureThisThreadInitialized();
        return rpaligned_realloc(Memory, Alignment, NewSize, 0, 0);
    }

    void Memory::FMalloc::Free(void* Memory)
    {
        EnsureThisThreadInitialized();
        rpfree(Memory);
    }

    size_t Memory::GetCurrentMappedMemory()    { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.mapped; }
    size_t Memory::GetPeakMappedMemory()       { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.mapped_peak; }
    size_t Memory::GetCachedMemory()           { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.cached; }
    size_t Memory::GetCurrentHugeAllocMemory() { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.huge_alloc; }
    size_t Memory::GetPeakHugeAllocMemory()    { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.huge_alloc_peak; }
    size_t Memory::GetTotalMappedMemory()      { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.mapped_total; }
    size_t Memory::GetTotalUnmappedMemory()    { rpmalloc_global_statistics_t s; rpmalloc_global_statistics(&s); return s.unmapped_total; }

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
        FMalloc& Allocator = (GMalloc != nullptr) ? *GMalloc : EnsureAllocator();
        void* pMemory = Allocator.Malloc(Size, Align<size_t>(Alignment, 16));
        LUMINA_PROFILE_ALLOC(pMemory, Size);
    #if LUMINA_MEMORY_TRACKING
        ::Lumina::Memory::Hooks::OnAlloc(pMemory, Size);
    #endif
        return pMemory;
    }

    void* Memory::Realloc(void* Memory, size_t NewSize, size_t Alignment)
    {
        FMalloc& Allocator = (GMalloc != nullptr) ? *GMalloc : EnsureAllocator();
        void* pMemory = Allocator.Realloc(Memory, NewSize, Align<size_t>(Alignment, 16));
    #if LUMINA_MEMORY_TRACKING
        ::Lumina::Memory::Hooks::OnRealloc(Memory, pMemory, NewSize);
    #endif
        return pMemory;
    }

    size_t Memory::GetAllocationSize(void* Memory)
    {
        return (Memory != nullptr) ? rpmalloc_usable_size(Memory) : 0;
    }

    bool Memory::TryExpandInPlace(void* Memory, size_t NewSize)
    {
        if (Memory == nullptr || rpmalloc_usable_size(Memory) < NewSize)
        {
            return false;
        }

    #if LUMINA_MEMORY_TRACKING
        ::Lumina::Memory::Hooks::OnRealloc(Memory, Memory, NewSize);
    #endif
        return true;
    }

    void Memory::Free(void*& Memory)
    {
        LUMINA_PROFILE_FREE(Memory);
    #if LUMINA_MEMORY_TRACKING
        ::Lumina::Memory::Hooks::OnFree(Memory);
    #endif
        FMalloc& Allocator = (GMalloc != nullptr) ? *GMalloc : EnsureAllocator();
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
        };

        FMutex       GFrameArenaMutex;
        FFrameArenaNode* GFrameArenaHead = nullptr;

        // The thread_local owner constructs the arena, links it in, and unlinks on thread exit.
        struct FThreadFrameArena
        {
            // A single container growth must fit one block, and chains beyond it on demand.
            static constexpr SIZE_T FrameBlockSize = 8 * 1024 * 1024;

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
            N->Arena->Reset();
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
