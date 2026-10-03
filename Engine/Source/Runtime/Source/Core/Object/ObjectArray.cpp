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
    }

    void FChunkedFixedCObjectArray::Shutdown()
    {
        if (Objects)
        {
            for (int32 i = 0; i < NumChunks; ++i)
            {
                if (Objects[i])
                {
                    Memory::DeleteArray(Objects[i]);
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

    void FChunkedFixedCObjectArray::EnsureChunkFor(int32 Index)
    {
        const int32 ChunkIndex = Index / NumElementsPerChunk;
        if (ChunkIndex < NumChunks)
        {
            return;
        }

        LUMINA_MEMORY_SCOPE("CObject");
        FScopeLock Lock(AllocationMutex);
        while (NumChunks <= ChunkIndex && NumChunks < MaxChunks)
        {
            Objects[NumChunks] = Memory::NewArray<FCObjectEntry>(NumElementsPerChunk);
            // A reader that sees the count must also see the zeroed chunk behind the pointer.
            std::atomic_thread_fence(std::memory_order_release);
            ++NumChunks;
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
        QuarantinedIndices.reserve(QuarantineCapacity);

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
            // Classified before anything dies, since IsA reads the class objects this pass goes on to free.
            TVector<TPair<int32, CObjectBase*>> Matched;
            ForEachObject([&Matched, bTypeObjects](CObjectBase* Object, int32 Index)
            {
                if (Object->IsA<CField>() == bTypeObjects)
                {
                    Matched.emplace_back(Index, Object);
                }
            });

            // An OnDestroy can free something else in the list, so the slot is rechecked before each step.
            for (const TPair<int32, CObjectBase*>& Entry : Matched)
            {
                if (GetObjectByIndex(Entry.first) == Entry.second)
                {
                    Entry.second->BeginDestroyForShutdown();
                }
            }

            for (const TPair<int32, CObjectBase*>& Entry : Matched)
            {
                if (GetObjectByIndex(Entry.first) == Entry.second)
                {
                    Entry.second->FinishDestroyForShutdown();
                }
            }
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
        QuarantinedIndices.clear();
        QuarantineCursor = 0;
        bInitialized = false;
    }

    FObjectHandle FCObjectArray::AllocateObject(CObjectBase* Object)
    {
        FRecursiveScopeLock Lock(Mutex);
        
        DEBUG_ASSERT(bInitialized && "Object pool not initialized!");
        DEBUG_ASSERT(Object != nullptr);
            
        int32 Index;
        int32 Generation;

        // Capacity beats the quarantine window, so give the delayed slots up rather than hit the ceiling.
        if (FreeIndices.empty() && !QuarantinedIndices.empty()
            && ChunkedArray.GetNumElements() >= ChunkedArray.GetMaxElements())
        {
            for (int32 Quarantined : QuarantinedIndices)
            {
                FreeIndices.push_back(Quarantined);
            }
            QuarantinedIndices.clear();
            QuarantineCursor = 0;
        }

        if (!FreeIndices.empty())
        {
            Index = FreeIndices.back();
            FreeIndices.pop_back();

            FCObjectEntry* Item = ChunkedArray.GetItem(Index);
            DEBUG_ASSERT(Item != nullptr);
            DEBUG_ASSERT(Item->GetObj() == nullptr);

            Item->IncrementGeneration();
            Generation = Item->GetGeneration();

            Item->ResetRefCount();
            Item->SetObj(Object);
        }
        else
        {
            Index = ChunkedArray.GetNumElements();

            // Strictly less, or a full pool passes the check and GetItem hands back the null it dereferences.
            ASSERT(Index < ChunkedArray.GetMaxElements(), "Object pool capacity exceeded!");
            ChunkedArray.EnsureChunkFor(Index);

            FCObjectEntry* Item = ChunkedArray.GetItem(Index);
            DEBUG_ASSERT(Item != nullptr);

            Generation = 1;
            Item->Generation.store(Generation, std::memory_order_release);
            Item->ResetRefCount();
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

        if ((int32)QuarantinedIndices.size() < QuarantineCapacity)
        {
            QuarantinedIndices.push_back(Index);
            return;
        }

        // A ring, since erasing the front of the vector would memmove the whole window on every free.
        FreeIndices.push_back(QuarantinedIndices[QuarantineCursor]);
        QuarantinedIndices[QuarantineCursor] = Index;
        QuarantineCursor = (QuarantineCursor + 1) % QuarantineCapacity;
    }

    void FCObjectArray::AddReinstanceRedirect(const FObjectHandle& From, const FObjectHandle& To)
    {
        if (!From.IsValid() || !To.IsValid() || From == To)
        {
            return;
        }

        TScopeLock<FRecursiveMutex> Lock(Mutex);
        ReinstanceRedirects.insert_or_assign(From, To);
    }

    CObjectBase* FCObjectArray::ResolveRedirect(const FObjectHandle& Handle) const
    {
        if (ReinstanceRedirects.empty())
        {
            return nullptr;
        }

        // An object reinstanced twice leaves a chain, and the bound keeps a cycle from hanging the resolve.
        FObjectHandle Current = Handle;
        for (int32 Hops = 0; Hops < 8; ++Hops)
        {
            const auto Found = ReinstanceRedirects.find(Current);
            if (Found == ReinstanceRedirects.end())
            {
                return nullptr;
            }

            Current = Found->second;

            const FCObjectEntry* Item = ChunkedArray.GetItem(Current.Index);
            if (Item != nullptr && Item->GetGeneration() == Current.Generation)
            {
                CObjectBase* Object = Item->GetObj();
                return (Object != nullptr && !Object->HasAnyFlag(OF_MarkedDestroy)) ? Object : nullptr;
            }
        }

        return nullptr;
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
            return ResolveRedirect(Handle);
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

        // Fatal outside Shipping, so the next one stops at the release site instead of accruing in a log.
        DEBUG_ASSERT(FScopedStaleReferenceTolerance::IsTolerated(),
            "Stale CObject reference released; see the error above for the slot and its current occupant.");
        return false;
    }

    void FCObjectArray::ReportUnbalancedRelease(const char* Site, const CObjectBase* Object) const
    {
        LOG_ERROR("FCObjectArray::{}: unbalanced release of {}; the entry's strong count was already zero or "
                  "the object was claimed for destruction. Destruction was skipped.",
            Site, (const void*)Object);

        DEBUG_ASSERT(FScopedStaleReferenceTolerance::IsTolerated(),
            "Unbalanced CObject release; a holder released a reference it did not own.");
    }

    FCObjectEntry* FCObjectArray::GetEntry(const CObjectBase* Object) const
    {
        if (Object == nullptr)
        {
            return nullptr;
        }

        FCObjectEntry* Item = const_cast<FCObjectArray*>(this)->ChunkedArray.GetItem(Object->GetInternalIndex());
        return (Item != nullptr && Item->GetObj() == Object) ? Item : nullptr;
    }

    bool FCObjectArray::ReleaseStrongRefEntry(FCObjectEntry* Entry, const CObjectBase* Expected)
    {
        // Shutdown destroys all objects manually; skip individual release.
        if (bShuttingDown || Entry == nullptr)
        {
            return false;
        }

        // Comparing addresses never dereferences Expected, so a dangling one is safe to ask about.
        if (Entry->GetObj() != Expected)
        {
            const CObjectBase* Occupant = Entry->GetObj();
            const CClass* OccupantClass = Occupant != nullptr ? Occupant->GetClass() : nullptr;
            LOG_ERROR("FCObjectArray::ReleaseStrongRefEntry: reference to freed object {} whose slot now holds "
                      "{} ('{}'). Dropped rather than charged to the new occupant.",
                (const void*)Expected, (const void*)Occupant,
                OccupantClass != nullptr ? OccupantClass->GetName().c_str() : "empty");
            return false;
        }

        int32 NewCount = 0;
        if (!Entry->ReleaseStrongRef(NewCount))
        {
            ReportUnbalancedRelease("ReleaseStrongRefEntry", Expected);
            return false;
        }

        if (NewCount != 0)
        {
            return false;
        }

        // Read after the decrement, since the entry is what stayed valid, not the caller's pointer.
        CObjectBase* Object = Entry->GetObj();
        return Object != nullptr && ConditionalDestroy(Object);
    }

    void FCObjectArray::AddStrongRef(CObjectBase* Object)
    {
        if (Object)
        {
            FCObjectEntry* Item = ChunkedArray.GetItem(Object->GetInternalIndex());
            if (Item != nullptr && OwnsSlot(Object, Item, "AddStrongRef"))
            {
                Item->AddStrongRefIfAlive();
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
                    ReportUnbalancedRelease("ReleaseStrongRef", Object);
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

        // Refused outright once the destroy is claimed, so there is no resurrection after free.
        if (!Item->AddStrongRefIfAlive())
        {
            return nullptr;
        }
        return Object;
    }

    bool FCObjectArray::ConditionalDestroy(CObjectBase* Object)
    {
        return DestroyClaimed(Object, false, "ConditionalDestroy");
    }

    bool FCObjectArray::ForceDestroy(CObjectBase* Object)
    {
        return DestroyClaimed(Object, true, "ForceDestroy");
    }

    bool FCObjectArray::TryClaimDestroyForShutdown(CObjectBase* Object)
    {
        if (Object == nullptr)
        {
            return false;
        }

        FCObjectEntry* Item = ChunkedArray.GetItem(Object->GetInternalIndex());
        if (Item == nullptr)
        {
            return true; // never registered, so the sweep is the only thing that can reach it
        }

        if (Item->GetObj() != Object)
        {
            return false; // freed already, and the slot has moved on
        }

        return Item->TryClaimDestroyForced();
    }

    bool FCObjectArray::DestroyClaimed(CObjectBase* Object, bool bForced, const char* Site)
    {
        if (Object == nullptr)
        {
            return false;
        }

        {
            FRecursiveScopeLock Lock(Mutex);

            FCObjectEntry* Item = ChunkedArray.GetItem(Object->GetInternalIndex());
            if (!OwnsSlot(Object, Item, Site))
            {
                return false; // freed already, and the slot has moved on
            }

            // The claim, not the lock, is what makes this exactly once across every destroy path.
            if (!(bForced ? Item->TryClaimDestroyForced() : Item->TryClaimDestroyIfUnreferenced()))
            {
                return false;
            }

            Object->SetFlag(OF_MarkedDestroy);
        }

        // Outside the lock, since OnDestroy runs arbitrary teardown.
        Object->DestroyInternal();
        return true;
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

    int32 FCObjectArray::GetNumAliveObjects() const
    {
        // Quarantined slots hold no object either, they are just held back from reuse for a while.
        return ChunkedArray.GetNumElements() - (int32)FreeIndices.size() - (int32)QuarantinedIndices.size();
    }

    int32 FCObjectArray::GetMaxObjects() const
    {
        return ChunkedArray.GetMaxElements();
    }

    namespace
    {
        thread_local int32 GStaleReferenceTolerance = 0;
    }

    FScopedStaleReferenceTolerance::FScopedStaleReferenceTolerance()
    {
        ++GStaleReferenceTolerance;
    }

    FScopedStaleReferenceTolerance::~FScopedStaleReferenceTolerance()
    {
        --GStaleReferenceTolerance;
    }

    bool FScopedStaleReferenceTolerance::IsTolerated()
    {
        return GStaleReferenceTolerance > 0;
    }
}
