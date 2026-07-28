#pragma once

#include "ObjectBase.h"
#include "ObjectHandle.h"
#include "Containers/Array.h"
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
        
        CObjectBase* Object = nullptr;
        TAtomic<int32> Generation{0};
        TAtomic<int32> StrongRefCount{0};
        TAtomic<int32> WeakRefCount{0};
        
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

        void AddStrongRef()
        {
            StrongRefCount.fetch_add(1, std::memory_order_relaxed);
        }

        uint32 ReleaseStrongRef()
        {
            int32 PrevCount = StrongRefCount.load(std::memory_order_relaxed);
            do
            {
                if (PrevCount <= 0)
                {
                    ASSERT(false, "ReleaseStrongRef on object with zero refcount");
                    return 0;
                }
            }
            while (!StrongRefCount.compare_exchange_weak(PrevCount, PrevCount - 1,
                std::memory_order_acq_rel, std::memory_order_relaxed));
            return uint32(PrevCount - 1);
        }

        void AddWeakRef()
        {
            WeakRefCount.fetch_add(1, std::memory_order_relaxed);
        }

        void ReleaseWeakRef()
        {
            WeakRefCount.fetch_sub(1, std::memory_order_relaxed);
        }

        int32 GetStrongRefCount() const
        {
            return StrongRefCount.load(std::memory_order_relaxed);
        }

        int32 GetWeakRefCount() const
        {
            return WeakRefCount.load(std::memory_order_relaxed);
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

        void ResetRefCounts()
        {
            StrongRefCount.store(0, std::memory_order_relaxed);
            WeakRefCount.store(0, std::memory_order_relaxed);
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
    
        void PreAllocateAllChunks();
    
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
        
        FRecursiveMutex             Mutex;
        FChunkedFixedCObjectArray   ChunkedArray;
        TVector<int32>              FreeIndices;
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

        RUNTIME_API CObjectBase* GetObjectByIndex(int32 Index) const;

        RUNTIME_API FObjectHandle GetHandleByObject(const CObjectBase* Object) const;

        RUNTIME_API FObjectHandle GetHandleByIndex(int32 Index) const;

        RUNTIME_API void AddStrongRef(CObjectBase* Object);

        /** Returns true if object was deleted. */
        RUNTIME_API bool ReleaseStrongRef(CObjectBase* Object);

        /** Validate a weak handle and acquire a strong ref in one atomic step (serialized with the
         *  destroy decision below). Returns the live object with its strong count already incremented,
         *  or nullptr if it was freed/reused. This is the only safe weak->strong upgrade. */
        RUNTIME_API CObjectBase* TryAddStrongRef(const FObjectHandle& Handle);

        /** Free Object iff it currently has no strong refs. The not-referenced check and the
         *  mark-for-destroy both run under the array lock, so a concurrent TryAddStrongRef either
         *  resurrects the object first (and this bails) or is refused afterwards, no UAF either way.
         *  Returns true if it freed. */
        RUNTIME_API bool ConditionalDestroy(CObjectBase* Object);

        RUNTIME_API void AddStrongRefByIndex(int32 Index);

        RUNTIME_API bool ReleaseStrongRefByIndex(int32 Index);

        RUNTIME_API void AddWeakRefByIndex(int32 Index);

        RUNTIME_API void ReleaseWeakRefByIndex(int32 Index);

        RUNTIME_API bool IsReferencedByIndex(int32 Index) const;

        RUNTIME_API int32 GetStrongRefCountByIndex(int32 Index) const;
        
        RUNTIME_API int32 GetWeakRefCountByIndex(uint32 Index) const;
    
        RUNTIME_API int32 GetNumAliveObjects() const;

        RUNTIME_API int32 GetMaxObjects() const;

        FORCEINLINE bool IsShuttingDown() const { return bShuttingDown; }
    
        template<typename Func>
        requires(eastl::is_invocable_v<Func, CObjectBase*, int32>)
        void ForEachObject(Func&& Function) const
        {
            const int32 MaxElements = ChunkedArray.GetNumElements();
            
            for (int32 i = 0; i < MaxElements; ++i)
            {
                const FCObjectEntry* Item = ChunkedArray.GetItem(i);
                if (Item && Item->GetObj())
                {
                    eastl::invoke(Function, Item->GetObj(), i);
                }
            }
        }
    };
    
    extern RUNTIME_API FCObjectArray GObjectArray;
    
}
