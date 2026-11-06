#pragma once

#include "ObjectReferenceInternal.h"
#include "ObjectIteration.h"
#include "Containers/Vector.h"
#include "Core/Threading/Thread.h"

namespace Lumina
{
    class CObjectBase;
}

namespace Lumina::Object::Internal
{
    // Global CObject control block that never reallocates.
    class FChunkedFixedCObjectArray
    {
    public:

        static constexpr int32 NumElementsPerChunk = 64 * 1024;

    private:

        FCObjectEntry** Objects = nullptr;
    
        int32 MaxElements = 0;
        std::atomic<int32> NumElements = 0;
        int32 MaxChunks = 0;
        int32 NumChunks = 0;
    
        FMutex AllocationMutex;
    
    public:
        
        FChunkedFixedCObjectArray() = default;
        ~FChunkedFixedCObjectArray()
        {
            Shutdown();
        }

        LE_NO_COPYMOVE(FChunkedFixedCObjectArray);

        
        void Initialize(int32 InMaxElements);
        
        void Shutdown();
    
        // Chunks are allocated as indices reach them, so the cap costs only the chunk pointer table up front.
        void EnsureChunkFor(int32 Index);
    
        const FCObjectEntry* GetItem(int32 Index) const;
    
        FCObjectEntry* GetItem(int32 Index);
    
        FORCEINLINE int32 GetMaxElements() const { return MaxElements; }
        FORCEINLINE int32 GetNumElements() const { return NumElements.load(std::memory_order_acquire); }
        FORCEINLINE int32 GetNumChunks() const { return NumChunks; }

        // An index never handed out before, or kIndexNone once every slot has been.
        int32 ClaimFreshIndex()
        {
            int32 Index = NumElements.load(std::memory_order_relaxed);
            do
            {
                if (Index >= MaxElements)
                {
                    return Constants::kIndexNone;
                }
            }
            while (!NumElements.compare_exchange_weak(Index, Index + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
            return Index;
        }
    };
        
    class FCObjectArray
    {
    private:
        
        // Delays slot reuse so a stale reference meets an empty slot, not an address-identical stranger.
        static constexpr int32      QuarantineCapacity = 1024;

        // Freed slots go back to the freeing thread's shard, so threads creating and destroying objects do not share a lock.
        static constexpr int32      SlotShardCount = 16;

        struct alignas(64) FSlotShard
        {
            FMutex          Mutex;
            TVector<int32>  FreeIndices;
            TVector<int32>  QuarantinedIndices;
            int32           QuarantineCursor = 0;
        };

        FRecursiveMutex             Mutex;
        FChunkedFixedCObjectArray   ChunkedArray;
        mutable FSlotShard          SlotShards[SlotShardCount];

        THashMap<FObjectHandle, FObjectHandle> ReinstanceRedirects;
        bool                        bInitialized = false;
        bool                        bShuttingDown = false;
    
    public:
        FCObjectArray() = default;
        ~FCObjectArray() = default;

        LE_NO_COPYMOVE(FCObjectArray);

        void AllocateObjectPool(int32 InMaxCObjects);

        // Arms the shutdown guard without tearing anything down, so releasing the root set stops
        // destroying objects individually and Shutdown() can do one ordered pass. Idempotent.
        void BeginShutdown();

        void Shutdown();

        FObjectHandle AllocateObject(CObjectBase* Object);

        void DeallocateObject(int32 Index);

        CObjectBase* ResolveHandle(const FObjectHandle& Handle) const;

        // Points every future resolve of From at To, so weak references follow a reinstanced object.
        void AddReinstanceRedirect(const FObjectHandle& From, const FObjectHandle& To);

    private:

        CObjectBase* ResolveRedirect(const FObjectHandle& Handle) const;

        FSlotShard& SlotShardForThisThread() const;

        // Only once no fresh index is left, since capacity beats the quarantine window.
        int32 ReclaimQuarantinedIndex();

    public:

        CObjectBase* GetObjectByIndex(int32 Index) const;

        FObjectHandle GetHandleByObject(const CObjectBase* Object) const;

        FObjectHandle GetHandleByIndex(int32 Index) const;

        // False once Object's slot has been recycled to someone else, so a stale reference is dropped not applied.
        bool OwnsSlot(const CObjectBase* Object, const FCObjectEntry* Item, const char* Site) const;

        void ReportUnbalancedRelease(const char* Site, const CObjectBase* Object) const;

        bool DestroyClaimed(CObjectBase* Object, bool bForced, const char* Site);

        // Entries outlive every object, so a reference caching one refcounts without touching object memory.
        FCObjectEntry* GetEntry(const CObjectBase* Object) const;

        // Releases through the entry rather than the object, and destroys at zero.
        bool ReleaseStrongRefEntry(FCObjectEntry* Entry, const CObjectBase* Expected);

        void AddStrongRef(CObjectBase* Object);

        bool ReleaseStrongRef(CObjectBase* Object);

        // The only safe weak to strong upgrade, serialized with the destroy decision, and null once the slot was freed or reused.
        CObjectBase* TryAddStrongRef(const FObjectHandle& Handle);

        // Frees Object iff nothing holds a strong ref, via one atomic claim only one caller can win.
        bool ConditionalDestroy(CObjectBase* Object);

        // Frees Object whatever its strong count says, still exactly once, for shutdown and reinstancing only.
        bool ForceDestroy(CObjectBase* Object);

        // Claims the destroy for the shutdown sweep, which frees the object itself rather than via ForceDestroy.
        bool TryClaimDestroyForShutdown(CObjectBase* Object);

        bool IsReferencedByIndex(int32 Index) const;

        int32 GetStrongRefCountByIndex(int32 Index) const;

        int32 GetNumAliveObjects() const;

        int32 GetMaxObjects() const;

        FORCEINLINE bool IsShuttingDown() const { return bShuttingDown; }
    
        template<typename Func>
        requires std::is_invocable_v<Func, CObjectBase*, int32>
        void ForEachObject(Func&& Function) const
        {
            const int32 MaxElements = ChunkedArray.GetNumElements();
            for (int32 Index = 0; Index < MaxElements; ++Index)
            {
                const FCObjectEntry* Entry = ChunkedArray.GetItem(Index);
                if (!Entry || !Entry->GetObj())
                {
                    continue;
                }
                if constexpr (std::is_same_v<std::invoke_result_t<Func, CObjectBase*, int32>, bool>)
                {
                    if (!Invoke(Function, Entry->GetObj(), Index))
                    {
                        break;
                    }
                }
                else
                {
                    Invoke(Function, Entry->GetObj(), Index);
                }
            }
        }
    };

    RUNTIME_API void AllocateObjectPool(int32 MaxObjects);
    RUNTIME_API void BeginShutdown();
    RUNTIME_API void Shutdown();
    RUNTIME_API FObjectHandle AllocateObject(CObjectBase* Object);
    RUNTIME_API void DeallocateObject(int32 Index);
    RUNTIME_API void AddReinstanceRedirect(const FObjectHandle& From, const FObjectHandle& To);
    RUNTIME_API FObjectHandle GetHandleByIndex(int32 Index);
    RUNTIME_API bool ReleaseStrongRef(CObjectBase* Object);
    RUNTIME_API bool ConditionalDestroy(CObjectBase* Object);
    RUNTIME_API bool ForceDestroy(CObjectBase* Object);
    RUNTIME_API bool TryClaimDestroyForShutdown(CObjectBase* Object);
    RUNTIME_API int32 GetStrongRefCountByIndex(int32 Index);
    RUNTIME_API int32 GetNumAliveObjects();
    RUNTIME_API int32 GetMaxObjects();
    RUNTIME_API bool IsShuttingDown();

    // Downgrades the stale-reference assert to a log for this thread, for tests that stage one on purpose.
    class FScopedStaleReferenceTolerance
    {
    public:
        RUNTIME_API FScopedStaleReferenceTolerance();
        RUNTIME_API ~FScopedStaleReferenceTolerance();

        LE_NO_COPYMOVE(FScopedStaleReferenceTolerance);

        static RUNTIME_API bool IsTolerated();
    };
}
