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

    FMaterialManager::FMaterialManager()
    {
        GrowLocked(kInitialMaterialSlots);
    }

    FMaterialManager::~FMaterialManager()
    {
        Core::Retire(MaterialBuffer);
        MaterialBuffer = 0;
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

    bool FMaterialManager::GrowLocked(uint32 MinSlots)
    {
        if (MinSlots <= Capacity)
        {
            return true;
        }

        if (Capacity >= kMaxMaterialSlots)
        {
            return false;
        }

        const uint32 NewCapacity = Math::Min(Math::Max(MinSlots, Capacity * 2), kMaxMaterialSlots);
        if (NewCapacity < MinSlots)
        {
            return false;
        }
        
        Mirror.resize(NewCapacity);

        const GPUPtr NewBuffer = Malloc(sizeof(FMaterialUniforms) * NewCapacity, kDefaultAlign, EMemoryType::GPUOnly);
        if (NewBuffer == 0)
        {
            LOG_ERROR("MaterialManager: failed to allocate a {} slot material table ({} KiB).",
                NewCapacity, (sizeof(FMaterialUniforms) * NewCapacity) / 1024);
            return false;
        }

        // The whole mirror in one upload, so every live slot survives the move at its existing index.
        UploadBuffer(NewBuffer, Mirror.data(), sizeof(FMaterialUniforms) * NewCapacity);
        
        Core::Retire(MaterialBuffer);

        MaterialBuffer = NewBuffer;
        Capacity = NewCapacity;

        return true;
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
    }
}
