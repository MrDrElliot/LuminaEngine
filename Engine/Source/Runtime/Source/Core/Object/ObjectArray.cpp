#include "RuntimePCH.h"
#include "ObjectArray.h"

#include "Class.h"          // CField, for the shutdown type-object ordering below
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"


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
        LUMINA_MEMORY_SCOPE("CObject");
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

        // Freed indices are recycled, so index order alone cannot keep a class alive past its instances.
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

        // An object already marked for destruction reads as gone, matching FindObject.
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

    // A freed object's slot is recycled immediately, so a stale reference would land on its successor.
    bool FCObjectArray::OwnsSlot(const CObjectBase* Object, const FCObjectEntry* Item, const char* Site) const
    {
        if (Item != nullptr && Item->GetObj() == Object)
        {
            return true;
        }

        const CObjectBase* Occupant = Item != nullptr ? Item->GetObj() : nullptr;
        const CClass* OccupantClass = Occupant != nullptr ? Occupant->GetClass() : nullptr;
        LOG_ERROR("FCObjectArray::{}: reference to freed object {} whose slot {} now holds {} ('{}'). "
                  "Something outlived the object it referenced; the reference was dropped instead of applied.",
            Site, (const void*)Object, Object->GetInternalIndex(), (const void*)Occupant,
            OccupantClass != nullptr ? OccupantClass->GetName().c_str() : "empty");
        return false;
    }

    void FCObjectArray::AddStrongRef(CObjectBase* Object)
    {
        if (Object)
        {
            FCObjectEntry* Item = ChunkedArray.GetItem(Object->GetInternalIndex());
            if (Item != nullptr && OwnsSlot(Object, Item, "AddStrongRef"))
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
            // The caller still holds the ref being dropped, so reading the index is safe and only the free locks.
            FCObjectEntry* Item = ChunkedArray.GetItem(Object->GetInternalIndex());
            if (Item != nullptr && OwnsSlot(Object, Item, "ReleaseStrongRef"))
            {
                int32 NewCount = 0;
                if (!Item->ReleaseStrongRef(NewCount))
                {
                    const CClass* Class = Object->GetClass();
                    LOG_ERROR("FCObjectArray::ReleaseStrongRef: unbalanced release of {} ('{}') at slot {}; "
                              "its strong count was already zero. Destruction was skipped.",
                        (const void*)Object, Class != nullptr ? Class->GetName().c_str() : "unknown",
                        Object->GetInternalIndex());
                    return false;
                }

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

        // The lock serializes against ConditionalDestroy, so there is no resurrection after free.
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

            const FCObjectEntry* Item = ChunkedArray.GetItem(Object->GetInternalIndex());
            if (!OwnsSlot(Object, Item, "ConditionalDestroy"))
            {
                return false; // freed already, and the slot has moved on
            }

            if (Object->HasAnyFlag(OF_MarkedDestroy))
            {
                return false; // already being destroyed
            }

            if (Item->IsReferenced())
            {
                return false; // (re)acquired a strong ref since the count hit zero, keep it alive
            }

            // Marked under the lock so a concurrent upgrade refuses, and torn down outside it to avoid deadlock.
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
            int32 NewCount = 0;
            if (!Item->ReleaseStrongRef(NewCount))
            {
                LOG_ERROR("FCObjectArray::ReleaseStrongRefByIndex: unbalanced release at slot {}; its strong "
                          "count was already zero. Destruction was skipped.", Index);
                return false;
            }

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
