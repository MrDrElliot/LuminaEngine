#include "RuntimePCH.h"
#include "MaterialManager.h"
#include "MaterialTypes.h"
#include "RHICore.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Log/Log.h"

namespace Lumina::RHI
{
    /** What the table starts at. Sized so a typical scene never grows at all. */
    static constexpr uint32 kInitialMaterialSlots = 1024;

    // The draw path carries the index as a uint16 and treats the maximum as no material.
    static constexpr uint32 kMaxMaterialSlots = 65535;

    // Slots kept in hand when a grow is staged, so the swap can land before the table is really full.
    static constexpr uint32 kGrowSlack = 64;

    FMaterialManager::FMaterialManager()
    {
        GrowLocked(kInitialMaterialSlots);
    }

    FMaterialManager::~FMaterialManager()
    {
        Retire(MaterialBuffer);
        Retire(PendingBuffer);
        MaterialBuffer = {};
        PendingBuffer  = {};
    }

    GPUPtr FMaterialManager::GetMaterialBuffer()
    {
        // Once per frame per view, which is the only place a staged table can be noticed to have landed.
        FWriteScopeLock Lock(Mutex);
        PublishPendingLocked();
        return MaterialBuffer.Gpu;
    }

    uint32 FMaterialManager::GetCapacity() const
    {
        FReadScopeLock Lock(Mutex);
        return Capacity;
    }

    uint32 FMaterialManager::GetNumMaterials() const
    {
        FReadScopeLock Lock(Mutex);
        return NumMaterials;
    }

    uint32 FMaterialManager::CopySlotTextureIDs(uint32 Index, uint32* OutIDs, uint32 MaxIDs) const
    {
        if (OutIDs == nullptr || MaxIDs == 0)
        {
            return 0;
        }

        FReadScopeLock Lock(Mutex);
        if (Index >= (uint32)Mirror.size())
        {
            return 0;
        }

        // The mirror IS what the shader reads, so there is no separate mapping to fall out of sync.
        const FMaterialUniforms& Uniforms = Mirror[Index];

        uint32 Count = 0;
        for (uint32 i = 0; i < MAX_TEXTURES && Count < MaxIDs; ++i)
        {
            const uint32 ID = Uniforms.Textures[i];
            if (ID == 0 || ID == ~0u)
            {
                continue;
            }

            // Materials repeat one packed texture across three channels, so de-duplicating keeps callers honest.
            bool bSeen = false;
            for (uint32 j = 0; j < Count; ++j)
            {
                bSeen = bSeen || (OutIDs[j] == ID);
            }
            if (!bSeen)
            {
                OutIDs[Count++] = ID;
            }
        }
        return Count;
    }

    void FMaterialManager::PublishPendingLocked()
    {
        if (PendingBuffer.Gpu == 0 || !Upload::IsBatchComplete(PendingBatch))
        {
            return;
        }

        // The old address only lives in scene roots, which are rebuilt every frame.
        Retire(MaterialBuffer);

        MaterialBuffer  = PendingBuffer;
        Capacity        = PendingCapacity;
        PendingBuffer   = {};
        PendingCapacity = 0;
        PendingBatch    = 0;
    }

    void FMaterialManager::StageGrowLocked(uint32 MinSlots)
    {
        if (PendingBuffer.Gpu != 0 || Capacity >= kMaxMaterialSlots)
        {
            return;
        }

        const uint32 Target = Math::Max(Math::Max(Capacity * 2, kInitialMaterialSlots), MinSlots);
        const uint32 NewCapacity = Math::Min(Target, kMaxMaterialSlots);
        if (NewCapacity <= Capacity)
        {
            return;
        }

        const FGPUAllocation NewBuffer = Malloc(sizeof(FMaterialUniforms) * NewCapacity, kDefaultAlign, EMemoryType::GPUOnly);
        if (NewBuffer.Gpu == 0)
        {
            LOG_ERROR("MaterialManager: failed to allocate a {} slot material table ({} KiB).",
                NewCapacity, (sizeof(FMaterialUniforms) * NewCapacity) / 1024);
            return;
        }

        // Sized with the staged table so a published slot always has a mirror entry; WriteSlotLocked still bounds itself by the PUBLISHED capacity.
        Mirror.resize(NewCapacity);

        // The whole mirror in one upload, so every live slot survives the move at its existing index.
        if (!UploadBuffer(NewBuffer, Mirror.data(), sizeof(FMaterialUniforms) * NewCapacity))
        {
            // Nothing was queued, so no batch names this copy and publishing would swap in raw Malloc bytes.
            LOG_ERROR("MaterialManager: the mirror copy into a {} slot table was dropped; the grow is abandoned.", NewCapacity);
            Retire(NewBuffer);
            return;
        }

        PendingBuffer   = NewBuffer;
        PendingCapacity = NewCapacity;
        // Read AFTER the upload is queued, so it names that upload's batch or a later one.
        PendingBatch    = Upload::BatchForQueuedOps();
    }

    bool FMaterialManager::GrowLocked(uint32 MinSlots)
    {
        PublishPendingLocked();

        if (MinSlots <= Capacity)
        {
            return true;
        }

        if (Capacity >= kMaxMaterialSlots)
        {
            return false;
        }

        if (PendingCapacity < MinSlots)
        {
            Retire(PendingBuffer);
            PendingBuffer   = {};
            PendingCapacity = 0;
            PendingBatch    = 0;
        }

        StageGrowLocked(MinSlots);

        // The slots are needed NOW, so this is the one path that waits. AddMaterial stages early to keep it rare.
        if (PendingBuffer.Gpu != 0)
        {
            FlushUploadsAndWait();
            PublishPendingLocked();
        }

        return MinSlots <= Capacity;
    }

    void FMaterialManager::AddMaterial(CMaterialInterface* Material)
    {
        FWriteScopeLock Lock(Mutex);

        DEBUG_ASSERT(Material != nullptr);
        DEBUG_ASSERT(Material->GetMaterialIndex() == -1);

        uint32 FreeIndex;

        if (!FreeList.empty())
        {
            FreeIndex = FreeList.back();
            FreeList.pop_back();
        }
        else
        {
            if (HighWater == Capacity && !GrowLocked(Capacity + 1))
            {
                LOG_ERROR("MaterialManager: material table is full at {} slots; '{}' will not render "
                    "with its own parameters.", Capacity, Material->GetName());
                return;
            }

            FreeIndex = HighWater++;
        }

        // Staged before the wall, so the copy has frames to land in and the grow above rarely waits.
        PublishPendingLocked();
        if (HighWater + kGrowSlack >= Capacity)
        {
            StageGrowLocked(HighWater + kGrowSlack + 1u);
        }

        ++NumMaterials;

        Material->SetMaterialIndex((int32)FreeIndex);
        WriteSlotLocked(Material->GetMaterialUniforms(), FreeIndex);
    }

    void FMaterialManager::RemoveMaterial(CMaterialInterface* Material)
    {
        DEBUG_ASSERT(Material != nullptr);
        DEBUG_ASSERT(Material->GetMaterialIndex() != -1);

        const int32 MaterialIndex = Material->GetMaterialIndex();

        // Once free another material can claim it, and an owner still naming it would write through it.
        Material->SetMaterialIndex(-1);

        RemoveMaterialSlot((uint32)MaterialIndex);
    }

    void FMaterialManager::RemoveMaterialSlot(uint32 Index)
    {
        FWriteScopeLock Lock(Mutex);

        // Safe only because this runs after the extract gate, when no recordable frame names the slot.
        WriteSlotLocked(nullptr, Index);

        FreeList.push_back(Index);
        --NumMaterials;
    }

    void FMaterialManager::UpdateMaterialUniforms(const FMaterialUniforms* InUniforms, uint32 Index)
    {
        FReadScopeLock Lock(Mutex);

        WriteSlotLocked(InUniforms, Index);
    }

    void FMaterialManager::UpdateMaterialUniformRange(uint32 Index, uint32 ByteOffset, const void* Data, uint32 ByteSize)
    {
        if (Data == nullptr || ByteSize == 0 || (uint64)ByteOffset + ByteSize > sizeof(FMaterialUniforms))
        {
            return;
        }

        // A read lock suffices for the same reason WriteSlotLocked takes one, only a grow moves storage.
        FReadScopeLock Lock(Mutex);

        if (Index >= Capacity)
        {
            return;
        }

        std::byte* Slot = reinterpret_cast<std::byte*>(&Mirror[Index]) + ByteOffset;
        if (Memory::MemEqual(Slot, Data, ByteSize))
        {
            return;
        }

        Memory::Memcpy(Slot, Data, ByteSize);

        UploadBuffer(MaterialBuffer, Data, ByteSize, Index * sizeof(FMaterialUniforms) + ByteOffset);

        // The staged table's mirror copy was queued before this write, so it has to be repeated there.
        if (PendingBuffer.Gpu != 0 && Index < PendingCapacity)
        {
            UploadBuffer(PendingBuffer, Data, ByteSize, Index * sizeof(FMaterialUniforms) + ByteOffset);
        }
    }

    void FMaterialManager::WriteSlotLocked(const FMaterialUniforms* InUniforms, uint32 Index)
    {
        if (Index >= Capacity)
        {
            return;
        }

        // Zeroed slot doubles as the "removed" state.
        FMaterialUniforms Copy{};
        if (InUniforms)
        {
            Copy = *InUniforms;
        }
        
        if (Memory::MemEqual(&Mirror[Index], &Copy))
        {
            return;
        }

        Mirror[Index] = Copy;

        UploadBuffer(MaterialBuffer, &Copy, sizeof(FMaterialUniforms), Index * sizeof(FMaterialUniforms));

        // The staged table's mirror copy was queued before this write, so it has to be repeated there.
        if (PendingBuffer.Gpu != 0 && Index < PendingCapacity)
        {
            UploadBuffer(PendingBuffer, &Copy, sizeof(FMaterialUniforms), Index * sizeof(FMaterialUniforms));
        }
    }

    FMaterialCollectionManager::FMaterialCollectionManager()
    {
        Mirror.resize(MAX_PARAMETER_COLLECTIONS);

        constexpr uint64 ByteSize = sizeof(FMaterialCollectionUniforms) * MAX_PARAMETER_COLLECTIONS;
        CollectionBuffer = Malloc(ByteSize, kDefaultAlign, EMemoryType::GPUOnly);
        if (CollectionBuffer.Gpu == 0)
        {
            LOG_ERROR("MaterialCollectionManager: failed to allocate the {} slot collection table ({} bytes).",
                MAX_PARAMETER_COLLECTIONS, ByteSize);
            return;
        }

        // Zeroed up front, which is also what makes reserved slot 0 read as an absent collection.
        UploadBuffer(CollectionBuffer, Mirror.data(), ByteSize);
    }

    FMaterialCollectionManager::~FMaterialCollectionManager()
    {
        Retire(CollectionBuffer);
        CollectionBuffer = {};
    }

    int32 FMaterialCollectionManager::Acquire()
    {
        FWriteScopeLock Lock(Mutex);

        if (!FreeList.empty())
        {
            const int32 Reused = FreeList.back();
            FreeList.pop_back();
            return Reused;
        }

        if (HighWater >= (int32)MAX_PARAMETER_COLLECTIONS)
        {
            LOG_ERROR("MaterialCollectionManager: the {} slot collection table is full; this collection "
                      "reads zeros for every parameter.", MAX_PARAMETER_COLLECTIONS);
            return INDEX_NONE;
        }

        return HighWater++;
    }

    void FMaterialCollectionManager::Release(int32 Index)
    {
        if (Index <= 0 || Index >= (int32)MAX_PARAMETER_COLLECTIONS)
        {
            return;
        }

        FWriteScopeLock Lock(Mutex);

        // Zeroed before it goes back, or the next owner reads the previous collection's values.
        Mirror[Index] = FMaterialCollectionUniforms{};
        UploadBuffer(CollectionBuffer, &Mirror[Index], sizeof(FMaterialCollectionUniforms),
            (uint64)Index * sizeof(FMaterialCollectionUniforms));

        FreeList.push_back(Index);
    }

    void FMaterialCollectionManager::Update(int32 Index, const FMaterialCollectionUniforms& Uniforms)
    {
        if (Index <= 0 || Index >= (int32)MAX_PARAMETER_COLLECTIONS)
        {
            return;
        }

        FWriteScopeLock Lock(Mutex);

        if (Memory::MemEqual(&Mirror[Index], &Uniforms))
        {
            return;
        }

        Mirror[Index] = Uniforms;
        UploadBuffer(CollectionBuffer, &Mirror[Index], sizeof(FMaterialCollectionUniforms),
            (uint64)Index * sizeof(FMaterialCollectionUniforms));
    }

    void FMaterialCollectionManager::UpdateRange(int32 Index, uint32 ByteOffset, const void* Data, uint32 ByteSize)
    {
        if (Index <= 0 || Index >= (int32)MAX_PARAMETER_COLLECTIONS || Data == nullptr || ByteSize == 0)
        {
            return;
        }

        if (ByteOffset + ByteSize > sizeof(FMaterialCollectionUniforms))
        {
            return;
        }

        FWriteScopeLock Lock(Mutex);

        std::byte* Slot = reinterpret_cast<std::byte*>(&Mirror[Index]) + ByteOffset;
        if (Memory::MemEqual(Slot, Data, ByteSize))
        {
            return;
        }

        Memory::Memcpy(Slot, Data, ByteSize);
        UploadBuffer(CollectionBuffer, Slot, ByteSize,
            (uint64)Index * sizeof(FMaterialCollectionUniforms) + ByteOffset);
    }
}
