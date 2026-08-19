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

    /**
     * The ceiling, and it is not arbitrary: the draw path carries the material index as a uint16
     * (FMeshResolveCache::FResolvedSurface::MaterialIdx), and ScenePrimitiveSet treats (uint16)-1 as
     * "no material". So 65534 is the highest index that can survive the trip to the GPU, and a table
     * one slot larger than that is the most that can ever be addressed.
     */
    static constexpr uint32 kMaxMaterialSlots = 65535;

    /** Slots kept in hand when a grow is staged, so the swap can land before the table is really full. */
    static constexpr uint32 kGrowSlack = 64;

    FMaterialManager::FMaterialManager()
    {
        GrowLocked(kInitialMaterialSlots);
    }

    FMaterialManager::~FMaterialManager()
    {
        Core::Retire(MaterialBuffer);
        Core::Retire(PendingBuffer);
        MaterialBuffer = 0;
        PendingBuffer  = 0;
    }

    GPUPtr FMaterialManager::GetMaterialBuffer() const
    {
        FReadScopeLock Lock(Mutex);
        return MaterialBuffer;
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

        // The mirror IS what the shader reads, so these are exactly the slots that material samples --
        // no separate mapping to fall out of sync, which is what made the previous material-keyed
        // attempt fail silently for anything missing from its map. A freed slot reads as zeroed, and
        // slot 0 is the magenta fallback, so a zero entry is correctly "no texture".
        const FMaterialUniforms& Uniforms = Mirror[Index];

        uint32 Count = 0;
        for (uint32 i = 0; i < MAX_TEXTURES && Count < MaxIDs; ++i)
        {
            const uint32 ID = Uniforms.Textures[i];
            if (ID == 0 || ID == ~0u)
            {
                continue;
            }

            // Materials repeat the same texture across channels constantly (one packed ORM feeding three
            // slots); de-duplicating here keeps the caller's inner loop honest.
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
        if (PendingBuffer == 0 || !Upload::IsBatchComplete(PendingBatch))
        {
            return;
        }

        // Plain fence lifetime: the old address only lives in scene roots, which are rebuilt every frame.
        Core::Retire(MaterialBuffer);

        MaterialBuffer  = PendingBuffer;
        Capacity        = PendingCapacity;
        PendingBuffer   = 0;
        PendingCapacity = 0;
        PendingBatch    = 0;
    }

    void FMaterialManager::StageGrowLocked()
    {
        if (PendingBuffer != 0 || Capacity >= kMaxMaterialSlots)
        {
            return;
        }

        const uint32 NewCapacity = Math::Min(Math::Max(Capacity * 2, kInitialMaterialSlots), kMaxMaterialSlots);
        if (NewCapacity <= Capacity)
        {
            return;
        }

        const GPUPtr NewBuffer = Malloc(sizeof(FMaterialUniforms) * NewCapacity, kDefaultAlign, EMemoryType::GPUOnly);
        if (NewBuffer == 0)
        {
            LOG_ERROR("MaterialManager: failed to allocate a {} slot material table ({} KiB).",
                NewCapacity, (sizeof(FMaterialUniforms) * NewCapacity) / 1024);
            return;
        }

        // Sized with the staged table so a published slot always has a mirror entry; WriteSlotLocked
        // still bounds itself by the PUBLISHED capacity, so the tail stays untouched until the swap.
        Mirror.resize(NewCapacity);

        // The whole mirror in one upload, so every live slot survives the move at its existing index.
        UploadBuffer(NewBuffer, Mirror.data(), sizeof(FMaterialUniforms) * NewCapacity);

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
            Core::Retire(PendingBuffer);
            PendingBuffer   = 0;
            PendingCapacity = 0;
            PendingBatch    = 0;
        }

        StageGrowLocked();

        // The slots are needed NOW and one past the published capacity would be written outside the
        // allocation, so this is the one path that waits. AddMaterial stages early to keep it rare.
        if (PendingBuffer != 0)
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
        if (HighWater + kGrowSlack >= Capacity)
        {
            PublishPendingLocked();
            StageGrowLocked();
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

        // Cleared BEFORE the slot goes back on the free list: once it is free another material can claim
        // it, and an owner still naming it would then write through someone else's slot.
        Material->SetMaterialIndex(-1);

        RemoveMaterialSlot((uint32)MaterialIndex);
    }

    void FMaterialManager::RemoveMaterialSlot(uint32 Index)
    {
        FWriteScopeLock Lock(Mutex);

        // Zeroing here is what makes an unreleased reference render the magenta placeholder (every
        // Textures[i] becomes 0, which is the 1x1 fallback's heap slot). That is intended as the terminal
        // state -- it is only safe because this runs after FRenderReleaseQueue's extract gate, i.e. once
        // no recorded or recordable frame still names the slot.
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

        // Read lock for the same reason WriteSlotLocked takes one: distinct slots are independent, and
        // only a grow moves the mirror's storage or the buffer address.
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

        UploadBuffer(MaterialBuffer + Index * sizeof(FMaterialUniforms) + ByteOffset, Data, ByteSize);

        // The staged table's mirror copy was queued before this write, so it has to be repeated there.
        if (PendingBuffer != 0 && Index < PendingCapacity)
        {
            UploadBuffer(PendingBuffer + Index * sizeof(FMaterialUniforms) + ByteOffset, Data, ByteSize);
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

        UploadBuffer(MaterialBuffer + Index * sizeof(FMaterialUniforms), &Copy, sizeof(FMaterialUniforms));

        // The staged table's mirror copy was queued before this write, so it has to be repeated there.
        if (PendingBuffer != 0 && Index < PendingCapacity)
        {
            UploadBuffer(PendingBuffer + Index * sizeof(FMaterialUniforms), &Copy, sizeof(FMaterialUniforms));
        }
    }
}
