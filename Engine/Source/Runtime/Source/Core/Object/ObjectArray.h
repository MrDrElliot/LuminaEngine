#pragma once

#include "Containers/Invoke.h"
#include "ObjectBase.h"
#include "ObjectHandle.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObjectBase;
}

namespace Lumina
{
    struct FCObjectEntry
    {
        FCObjectEntry() = default;

        // The destroy claim lives in the count as a negative sentinel, so acquiring and claiming are one CAS.
        static constexpr int32 DestroyClaimed = INT32_MIN;

        CObjectBase* Object = nullptr;
        TAtomic<int32> Generation{0};
        TAtomic<int32> StrongRefCount{0};

        FCObjectEntry(FCObjectEntry&&) = delete;
        FCObjectEntry(const FCObjectEntry&) = delete;
        FCObjectEntry& operator=(FCObjectEntry&&) = delete;
        FCObjectEntry& operator=(const FCObjectEntry&) = delete;

        FORCEINLINE CObjectBase* GetObj() const
        {
            return Object;
        }

        FORCEINLINE void SetObj(CObjectBase* InObject)
        {
            Object = InObject;
        }

        // Refuses an object already claimed for destruction, so no acquire can resurrect one.
        bool AddStrongRefIfAlive()
        {
            int32 PrevCount = StrongRefCount.load(std::memory_order_acquire);
            do
            {
                if (PrevCount < 0)
                {
                    return false;
                }
            }
            while (!StrongRefCount.compare_exchange_weak(PrevCount, PrevCount + 1,
                std::memory_order_acq_rel, std::memory_order_acquire));
            return true;
        }

        // Returns false for an unbalanced release, which a zero count alone cannot be told apart from.
        bool ReleaseStrongRef(int32& OutNewCount)
        {
            int32 PrevCount = StrongRefCount.load(std::memory_order_relaxed);
            do
            {
                if (PrevCount <= 0)
                {
                    OutNewCount = 0;
                    return false;
                }
            }
            while (!StrongRefCount.compare_exchange_weak(PrevCount, PrevCount - 1,
                std::memory_order_acq_rel, std::memory_order_relaxed));
            OutNewCount = PrevCount - 1;
            return true;
        }

        // Wins only from exactly zero, and only once, so this alone is the destroy decision.
        bool TryClaimDestroyIfUnreferenced()
        {
            int32 Expected = 0;
            return StrongRefCount.compare_exchange_strong(Expected, DestroyClaimed,
                std::memory_order_acq_rel, std::memory_order_relaxed);
        }

        // The force and shutdown paths, which destroy whatever the count says, still exactly once.
        bool TryClaimDestroyForced()
        {
            int32 PrevCount = StrongRefCount.load(std::memory_order_relaxed);
            do
            {
                if (PrevCount == DestroyClaimed)
                {
                    return false;
                }
            }
            while (!StrongRefCount.compare_exchange_weak(PrevCount, DestroyClaimed,
                std::memory_order_acq_rel, std::memory_order_relaxed));
            return true;
        }

        // Clamped, since the claim sentinel is an implementation detail no caller should be shown.
        int32 GetStrongRefCount() const
        {
            const int32 Count = StrongRefCount.load(std::memory_order_relaxed);
            return Count > 0 ? Count : 0;
        }

        int32 GetGeneration() const
        {
            return Generation.load(std::memory_order_acquire);
        }

        void IncrementGeneration()
        {
            Generation.fetch_add(1, std::memory_order_release);
        }

        bool IsReferenced() const
        {
            return StrongRefCount.load(std::memory_order_relaxed) > 0;
        }

        // Also clears any destroy claim, which is what makes a recycled slot usable again.
        void ResetRefCount()
        {
            StrongRefCount.store(0, std::memory_order_relaxed);
        }
    };

    /** Global CObject control block. Never reallocates. */
    class FChunkedFixedCObjectArray
    {
    public:

        static constexpr int32 NumElementsPerChunk = 64 * 1024;

    private:

        FCObjectEntry** Objects = nullptr;
    
        int32 MaxElements = 0;
        int32 NumElements = 0;
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
    
        RUNTIME_API const FCObjectEntry* GetItem(int32 Index) const;
    
        RUNTIME_API FCObjectEntry* GetItem(int32 Index);
    
        FORCEINLINE int32 GetMaxElements() const { return MaxElements; }
        FORCEINLINE int32 GetNumElements() const { return NumElements; }
        FORCEINLINE int32 GetNumChunks() const { return NumChunks; }
    
        FORCEINLINE void IncrementElementCount()
        {
            ++NumElements;
        }

        FORCEINLINE void DecrementElementCount()
        {
            --NumElements;
        }
    
    };
        
    class FCObjectArray
    {
    private:
        
        // Delays slot reuse so a stale reference meets an empty slot, not an address-identical stranger.
        static constexpr int32      QuarantineCapacity = 1024;

        FRecursiveMutex             Mutex;
        FChunkedFixedCObjectArray   ChunkedArray;
        TVector<int32>              FreeIndices;
        TVector<int32>              QuarantinedIndices;
        int32                       QuarantineCursor = 0;

        // A reinstanced object did not die, it became another object, so a handle to the original resolves
        // onward instead of reading as destroyed. Consulted only when the generation already failed to
        // match, so a live handle never pays for it.
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

        RUNTIME_API CObjectBase* ResolveHandle(const FObjectHandle& Handle) const;

        /** Points every future resolve of From at To, so weak references follow a reinstanced object. */
        RUNTIME_API void AddReinstanceRedirect(const FObjectHandle& From, const FObjectHandle& To);

    private:

        CObjectBase* ResolveRedirect(const FObjectHandle& Handle) const;

    public:

        RUNTIME_API CObjectBase* GetObjectByIndex(int32 Index) const;

        RUNTIME_API FObjectHandle GetHandleByObject(const CObjectBase* Object) const;

        RUNTIME_API FObjectHandle GetHandleByIndex(int32 Index) const;

        // False once Object's slot has been recycled to someone else, so a stale reference is dropped not applied.
        bool OwnsSlot(const CObjectBase* Object, const FCObjectEntry* Item, const char* Site) const;

        void ReportUnbalancedRelease(const char* Site, const CObjectBase* Object) const;

        bool DestroyClaimed(CObjectBase* Object, bool bForced, const char* Site);

        // Entries outlive every object, so a reference caching one refcounts without touching object memory.
        RUNTIME_API FCObjectEntry* GetEntry(const CObjectBase* Object) const;

        // Releases through the entry rather than the object, and destroys at zero.
        RUNTIME_API bool ReleaseStrongRefEntry(FCObjectEntry* Entry, const CObjectBase* Expected);

        RUNTIME_API void AddStrongRef(CObjectBase* Object);

        /** Returns true if object was deleted. */
        RUNTIME_API bool ReleaseStrongRef(CObjectBase* Object);

        /** Validate a weak handle and acquire a strong ref in one atomic step (serialized with the
         *  destroy decision below). Returns the live object with its strong count already incremented,
         *  or nullptr if it was freed/reused. This is the only safe weak->strong upgrade. */
        RUNTIME_API CObjectBase* TryAddStrongRef(const FObjectHandle& Handle);

        /** Frees Object iff nothing holds a strong ref, via one atomic claim only one caller can win. */
        RUNTIME_API bool ConditionalDestroy(CObjectBase* Object);

        /** Frees Object whatever its strong count says, still exactly once. Shutdown and reinstancing only. */
        RUNTIME_API bool ForceDestroy(CObjectBase* Object);

        /** Claims the destroy for the shutdown sweep, which frees the object itself rather than via above. */
        RUNTIME_API bool TryClaimDestroyForShutdown(CObjectBase* Object);

        RUNTIME_API bool IsReferencedByIndex(int32 Index) const;

        RUNTIME_API int32 GetStrongRefCountByIndex(int32 Index) const;

        RUNTIME_API int32 GetNumAliveObjects() const;

        RUNTIME_API int32 GetMaxObjects() const;

        FORCEINLINE bool IsShuttingDown() const { return bShuttingDown; }
    
        template<typename Func>
        requires(std::is_invocable_v<Func, CObjectBase*, int32>)
        void ForEachObject(Func&& Function) const
        {
            const int32 MaxElements = ChunkedArray.GetNumElements();
            
            for (int32 i = 0; i < MaxElements; ++i)
            {
                const FCObjectEntry* Item = ChunkedArray.GetItem(i);
                if (Item && Item->GetObj())
                {
                    Invoke(Function, Item->GetObj(), i);
                }
            }
        }
    };
    
    extern RUNTIME_API FCObjectArray GObjectArray;

    /** Downgrades the stale-reference assert to a log for this thread, for tests that stage one on purpose. */
    class FScopedStaleReferenceTolerance
    {
    public:
        RUNTIME_API FScopedStaleReferenceTolerance();
        RUNTIME_API ~FScopedStaleReferenceTolerance();

        LE_NO_COPYMOVE(FScopedStaleReferenceTolerance);

        static RUNTIME_API bool IsTolerated();
    };
}
