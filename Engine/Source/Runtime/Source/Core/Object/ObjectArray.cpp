#include "RuntimePCH.h"
#include "ObjectArray.h"

#include "Class.h"          // CField, for the shutdown type-object ordering below
#include "Memory/Memory.h"


namespace Lumina
{
    void FChunkedFixedCObjectArray::Initialize(int32 InMaxElements)
    {
        DEBUG_ASSERT(Objects == nullptr, "Already initialized!");
        DEBUG_ASSERT(InMaxElements > 100);
    
        MaxElements = InMaxElements;
        MaxChunks   = (InMaxElements + NumElementsPerChunk - 1) / NumElementsPerChunk;
        NumChunks   = 0;
        NumElements = 0;
    
        Objects = Memory::NewArray<FCObjectEntry*>(MaxChunks);
        Memory::Memzero(Objects, MaxChunks * sizeof(FCObjectEntry*));  // NOLINT(bugprone-multi-level-implicit-pointer-conversion)
            
        PreAllocateAllChunks();
    }

    void FChunkedFixedCObjectArray::Shutdown()
    {
        if (Objects)
        {
            for (int32 i = 0; i < NumChunks; ++i)
            {
                if (Objects[i])
                {
                    Memory::Delete(Objects[i]);
                    Objects[i] = nullptr;
                }
            }
    
            Memory::DeleteArray(Objects);
            Objects = nullptr;
        }
    
        MaxElements = 0;
        NumElements = 0;
        MaxChunks = 0;
        NumChunks = 0;
    }

    void FChunkedFixedCObjectArray::PreAllocateAllChunks()
    {
        FScopeLock Lock(AllocationMutex);
    
        for (int32 ChunkIndex = 0; ChunkIndex < MaxChunks; ++ChunkIndex)
        {
            if (!Objects[ChunkIndex])
            {
                Objects[ChunkIndex] = Memory::NewArray<FCObjectEntry>(NumElementsPerChunk);
                NumChunks = ChunkIndex + 1;
            }
        }
    }

    const FCObjectEntry* FChunkedFixedCObjectArray::GetItem(int32 Index) const
    {
        if (Index < 0 || Index >= MaxElements)
        {
            return nullptr;
        }
    
        const int32 ChunkIndex = Index / NumElementsPerChunk;
        const int32 SubIndex = Index % NumElementsPerChunk;
    
        if (ChunkIndex >= NumChunks || !Objects[ChunkIndex])
        {
            return nullptr;
        }
    
        return &Objects[ChunkIndex][SubIndex];
    }

    FCObjectEntry* FChunkedFixedCObjectArray::GetItem(int32 Index)
    {
        if (Index < 0 || Index >= MaxElements)
        {
            return nullptr;
        }
    
        const int32 ChunkIndex = Index / NumElementsPerChunk;
        const int32 SubIndex = Index % NumElementsPerChunk;
            
        if (ChunkIndex >= NumChunks || !Objects[ChunkIndex])
        {
            return nullptr;
        }
            
        return &Objects[ChunkIndex][SubIndex];
    }

    void FCObjectArray::AllocateObjectPool(int32 InMaxCObjects)
    {
        DEBUG_ASSERT(!bInitialized && "Object pool already allocated!");

        const int32 MaxObjects = Math::Max(1000, InMaxCObjects);
        
        ChunkedArray.Initialize(MaxObjects);
            
        FreeIndices.reserve(MaxObjects / 4);
    
        bInitialized = true;
    }

    void FCObjectArray::BeginShutdown()
    {
        FRecursiveScopeLock Lock(Mutex);
        bShuttingDown = true;
    }

    void FCObjectArray::Shutdown()
    {
        FRecursiveScopeLock Lock(Mutex);

        bShuttingDown = true;

        // Types outlive their instances. An instance's teardown is entitled to ask for its class -- a
        // minted script class's destructor reaches through it to destruct the appended trailing block --
        // so freeing a class while its instances are still alive is a use-after-free. Index order alone
        // does not give that ordering: freed indices are recycled, so an object created late can sit at a
        // lower index than the class it was made from.
        //
        // CField covers CClass / CStruct / CEnum, i.e. everything that can be another object's class or
        // describe its layout. Classifying is safe here because nothing has been freed yet.
        auto DestroyPass = [this](bool bTypeObjects)
        {
            ForEachObject([bTypeObjects](CObjectBase* Object, int32)
            {
                if (Object->IsA<CField>() == bTypeObjects)
                {
                    Object->BeginDestroyForShutdown();
                }
            });

            ForEachObject([bTypeObjects](CObjectBase* Object, int32)
            {
                if (Object->IsA<CField>() == bTypeObjects)
                {
                    Object->FinishDestroyForShutdown();
                }
            });
        };

        DestroyPass(/*bTypeObjects*/ false);
        DestroyPass(/*bTypeObjects*/ true);

        // Anything the two passes created on their way out (a type object minted during an OnDestroy, say).
        ForEachObject([](CObjectBase* Object, int32)
        {
            Object->BeginDestroyForShutdown();
        });

        ForEachObject([](CObjectBase* Object, int32)
        {
            Object->FinishDestroyForShutdown();
        });

        ChunkedArray.Shutdown();
        FreeIndices.clear();
        bInitialized = false;
    }

    FObjectHandle FCObjectArray::AllocateObject(CObjectBase* Object)
    {
        FRecursiveScopeLock Lock(Mutex);
        
        DEBUG_ASSERT(bInitialized && "Object pool not initialized!");
        DEBUG_ASSERT(Object != nullptr);
            
        int32 Index;
        int32 Generation;
    
        if (!FreeIndices.empty())
        {
            Index = FreeIndices.back();
            FreeIndices.pop_back();

            FCObjectEntry* Item = ChunkedArray.GetItem(Index);
            DEBUG_ASSERT(Item != nullptr);
            DEBUG_ASSERT(Item->GetObj() == nullptr);

            Item->IncrementGeneration();
            Generation = Item->GetGeneration();

            Item->ResetRefCounts();
            Item->SetObj(Object);
        }
        else
        {
            Index = ChunkedArray.GetNumElements();

            ASSERT(Index <= ChunkedArray.GetMaxElements(), "Object pool capacity exceeded!");

            FCObjectEntry* Item = ChunkedArray.GetItem(Index);
            DEBUG_ASSERT(Item != nullptr);

            Generation = 1;
            Item->Generation.store(Generation, std::memory_order_release);
            Item->ResetRefCounts();
            Item->SetObj(Object);

            ChunkedArray.IncrementElementCount();
        }
    

        return FObjectHandle(Index, Generation);
    }

    void FCObjectArray::DeallocateObject(int32 Index)
    {
        FRecursiveScopeLock Lock(Mutex);

        DEBUG_ASSERT(bInitialized, "Object pool not initialized!");
            
        FCObjectEntry* Item = ChunkedArray.GetItem(Index);
        DEBUG_ASSERT(Item != nullptr);
        DEBUG_ASSERT(Item->GetObj() != nullptr);
    
        Item->SetObj(nullptr);
    
        Item->IncrementGeneration();
    
        FreeIndices.push_back(Index);
    
    }

    CObjectBase* FCObjectArray::ResolveHandle(const FObjectHandle& Handle) const
    {
        if (!Handle.IsValid())
        {
            return nullptr;
        }
    
        const FCObjectEntry* Item = ChunkedArray.GetItem(Handle.Index);
        if (!Item)
        {
            return nullptr;
        }
    
        const int32 Generation = Item->GetGeneration();
        if (Generation != Handle.Generation)
        {
            return nullptr;
        }

        // Generation matches, so the slot still holds this object (not yet deallocated), the pointer is
        // safe to read and to test the flag on. Treat an object already marked for destruction as gone,
        // matching FindObject and so weak resolves don't hand back a dying object.
        CObjectBase* Object = Item->GetObj();
        if (Object == nullptr || Object->HasAnyFlag(OF_MarkedDestroy))
        {
            return nullptr;
        }
        return Object;
    }

    CObjectBase* FCObjectArray::GetObjectByIndex(int32 Index) const
    {
        const FCObjectEntry* Item = ChunkedArray.GetItem(Index);
        return Item ? Item->GetObj() : nullptr;
    }

    FObjectHandle FCObjectArray::GetHandleByObject(const CObjectBase* Object) const
    {
        return GetHandleByIndex(Object->GetInternalIndex());
    }

    FObjectHandle FCObjectArray::GetHandleByIndex(int32 Index) const
    {
        const FCObjectEntry* Item = ChunkedArray.GetItem(Index);
        if (!Item || !Item->GetObj())
        {
            return FObjectHandle();
        }
    
        return FObjectHandle(Index, Item->GetGeneration());
    }

    void FCObjectArray::AddStrongRef(CObjectBase* Object)
    {
        if (Object)
        {
            if (FCObjectEntry* Item = ChunkedArray.GetItem(Object->GetInternalIndex()))
            {
                Item->AddStrongRef();
            }
        }
    }

    bool FCObjectArray::ReleaseStrongRef(CObjectBase* Object)
    {
        // Shutdown destroys all objects manually; skip individual release.
        if (bShuttingDown)
        {
            return false;
        }

        if (Object)
        {
            // The caller still holds the ref being dropped, so the object is alive here, reading its
            // index is safe. The decrement is lock-free; only the at-zero free takes the lock.
            if (FCObjectEntry* Item = ChunkedArray.GetItem(Object->GetInternalIndex()))
            {
                uint32 NewCount = Item->ReleaseStrongRef();
                if (NewCount == 0)
                {
                    return ConditionalDestroy(Object);
                }
            }
        }
        return false;
    }

    CObjectBase* FCObjectArray::TryAddStrongRef(const FObjectHandle& Handle)
    {
        if (!Handle.IsValid())
        {
            return nullptr;
        }

        FRecursiveScopeLock Lock(Mutex);

        FCObjectEntry* Item = ChunkedArray.GetItem(Handle.Index);
        if (!Item || Item->GetGeneration() != Handle.Generation)
        {
            return nullptr; // freed and the slot moved on (or never existed)
        }

        CObjectBase* Object = Item->GetObj();
        if (Object == nullptr || Object->HasAnyFlag(OF_MarkedDestroy))
        {
            return nullptr; // being destroyed
        }

        // Holding the lock here serializes against ConditionalDestroy: it either sees this incremented
        // count and bails, or already marked the object (caught above). No resurrection-after-free.
        Item->AddStrongRef();
        return Object;
    }

    bool FCObjectArray::ConditionalDestroy(CObjectBase* Object)
    {
        if (Object == nullptr)
        {
            return false;
        }

        {
            FRecursiveScopeLock Lock(Mutex);

            if (Object->HasAnyFlag(OF_MarkedDestroy))
            {
                return false; // already being destroyed
            }

            const FCObjectEntry* Item = ChunkedArray.GetItem(Object->GetInternalIndex());
            if (Item && Item->IsReferenced())
            {
                return false; // (re)acquired a strong ref since the count hit zero, keep it alive
            }

            // Mark under the lock so any concurrent TryAddStrongRef now refuses. Once marked we own the
            // destruction; the actual teardown runs outside the lock so OnDestroy can't deadlock on it.
            Object->SetFlag(OF_MarkedDestroy);
        }

        Object->DestroyInternal();
        return true;
    }

    void FCObjectArray::AddStrongRefByIndex(int32 Index)
    {
        if (FCObjectEntry* Item = ChunkedArray.GetItem(Index))
        {
            Item->AddStrongRef();
        }
    }

    bool FCObjectArray::ReleaseStrongRefByIndex(int32 Index)
    {
        if (bShuttingDown)
        {
            return false;
        }
        if (FCObjectEntry* Item = ChunkedArray.GetItem(Index))
        {
            uint32 NewCount = Item->ReleaseStrongRef();
            if (NewCount == 0)
            {
                if (CObjectBase* Object = Item->GetObj())
                {
                    return ConditionalDestroy(Object);
                }
            }
        }
        return false;
    }

    void FCObjectArray::AddWeakRefByIndex(int32 Index)
    {
        if (FCObjectEntry* Item = ChunkedArray.GetItem(Index))
        {
            Item->AddWeakRef();
        }
    }

    void FCObjectArray::ReleaseWeakRefByIndex(int32 Index)
    {
        if (FCObjectEntry* Item = ChunkedArray.GetItem(Index))
        {
            Item->ReleaseWeakRef();
        }
    }

    bool FCObjectArray::IsReferencedByIndex(int32 Index) const
    {
        const FCObjectEntry* Item = ChunkedArray.GetItem(Index);
        return Item && Item->IsReferenced();
    }

    int32 FCObjectArray::GetStrongRefCountByIndex(int32 Index) const
    {
        const FCObjectEntry* Item = ChunkedArray.GetItem(Index);
        return Item ? Item->GetStrongRefCount() : 0;
    }

    int32 FCObjectArray::GetWeakRefCountByIndex(uint32 Index) const
    {
        const FCObjectEntry* Item = ChunkedArray.GetItem(Index);
        return Item ? Item->GetWeakRefCount() : 0;
    }

    int32 FCObjectArray::GetNumAliveObjects() const
    {
        return ChunkedArray.GetNumElements() - (int32)FreeIndices.size();
    }

    int32 FCObjectArray::GetMaxObjects() const
    {
        return ChunkedArray.GetMaxElements();
    }
}
