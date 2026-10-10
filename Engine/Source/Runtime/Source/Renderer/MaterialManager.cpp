#include "RuntimePCH.h"
#include "MaterialManager.h"
#include "MaterialTypes.h"
#include "RHICore.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Log/Log.h"

namespace Lumina::RHI
{
    // Sized so a typical scene never grows at all.
    static constexpr uint32 kInitialMaterialSlots = 1024;
    static constexpr uint32 kInitialPoolUnits     = 16384;

    // The draw path carries the index as a uint16 and treats the maximum as no material.
    static constexpr uint32 kMaxMaterialSlots = 65535;

    // Kept in hand when a grow is staged, so the swap can land before the table is really full.
    static constexpr uint32 kGrowSlack     = 64;
    static constexpr uint32 kPoolGrowSlack = 2048;

    namespace
    {
        struct FPackedLayout
        {
            uint32 NumVectors  = 0;
            uint32 NumScalars  = 0;
            uint32 NumTextures = 0;

            uint32 Words() const { return NumVectors * 4u + NumScalars + NumTextures; }
            uint32 Units() const { return (Words() + 3u) / 4u; }
            bool operator==(const FPackedLayout&) const = default;
        };

        // Entries past the last nonzero one read back as zero on the GPU, so the block can stop there.
        template<typename T, uint32 N>
        uint32 CountToLastNonZero(const T (&Values)[N])
        {
            const T Zero = {};
            for (uint32 i = N; i > 0; --i)
            {
                if (!Memory::MemEqual(&Values[i - 1], &Zero))
                {
                    return i;
                }
            }
            return 0;
        }

        FPackedLayout MeasureLayout(const FMaterialUniforms& U)
        {
            return { CountToLastNonZero(U.Vectors), CountToLastNonZero(U.Scalars), CountToLastNonZero(U.Textures) };
        }

        FPackedLayout UnpackLayout(uint32 Layout)
        {
            return { Layout & MATERIAL_COUNT_MASK,
                     (Layout >> MATERIAL_SCALAR_COUNT_SHIFT) & MATERIAL_COUNT_MASK,
                     (Layout >> MATERIAL_TEXTURE_COUNT_SHIFT) & MATERIAL_COUNT_MASK };
        }

        FMaterialHeaderGPU MakeHeader(const FMaterialUniforms& U, const FPackedLayout& L, uint32 DataWord)
        {
            uint32 Layout = L.NumVectors | (L.NumScalars << MATERIAL_SCALAR_COUNT_SHIFT) | (L.NumTextures << MATERIAL_TEXTURE_COUNT_SHIFT);
            for (uint32 i = 0; i < MAX_MATERIAL_COLLECTIONS; ++i)
            {
                Layout |= (U.CollectionIndices[i] & MATERIAL_COLLECTION_MASK) << (MATERIAL_COLLECTION_SHIFT + i * MATERIAL_COLLECTION_BITS);
            }
            return { U.Flags, U.OpacityClipValue, DataWord, Layout };
        }
    }

    void FMirroredGPUBuffer::Write(uint64 Offset, const void* Data, uint64 Size)
    {
        if (Offset + Size <= LiveBytes)
        {
            UploadBuffer(Live, Data, Size, Offset);
        }

        // The staged copy was queued before this write, so it has to be repeated there.
        if (Pending.Gpu != 0 && Offset + Size <= PendingBytes)
        {
            UploadBuffer(Pending, Data, Size, Offset);
        }
    }

    bool FMirroredGPUBuffer::Stage(const void* Image, uint64 Bytes)
    {
        const FGPUAllocation NewBuffer = Malloc(Bytes, kDefaultAlign, EMemoryType::GPUOnly);
        if (NewBuffer.Gpu == 0)
        {
            LOG_ERROR("MaterialManager: failed to allocate {} KiB for a grown material buffer.", Bytes / 1024);
            return false;
        }

        if (!UploadBuffer(NewBuffer, Image, Bytes))
        {
            // Nothing was queued, so no batch names this copy and publishing would swap in raw Malloc bytes.
            LOG_ERROR("MaterialManager: the image copy into a {} KiB buffer was dropped, so the grow is abandoned.", Bytes / 1024);
            Retire(NewBuffer);
            return false;
        }

        Pending      = NewBuffer;
        PendingBytes = Bytes;
        // Read after the upload is queued, so it names that upload's batch or a later one.
        PendingBatch = Upload::BatchForQueuedOps();
        return true;
    }

    void FMirroredGPUBuffer::Publish()
    {
        if (Pending.Gpu == 0 || !Upload::IsBatchComplete(PendingBatch))
        {
            return;
        }

        // The old address only lives in scene roots, which are rebuilt every frame.
        Retire(Live);
        Live         = Pending;
        LiveBytes    = PendingBytes;
        Pending      = {};
        PendingBytes = 0;
        PendingBatch = 0;
    }

    void FMirroredGPUBuffer::DropPending()
    {
        Retire(Pending);
        Pending      = {};
        PendingBytes = 0;
        PendingBatch = 0;
    }

    void FMirroredGPUBuffer::Release()
    {
        DropPending();
        Retire(Live);
        Live      = {};
        LiveBytes = 0;
    }

    FMaterialManager::FMaterialManager()
    {
        GrowLocked(kInitialMaterialSlots);
        GrowPoolLocked(kInitialPoolUnits);
    }

    FMaterialManager::~FMaterialManager()
    {
        Headers.Release();
        Pool.Release();
    }

    FMaterialTableGPU FMaterialManager::GetMaterialTable()
    {
        // Once per frame per view, which is the only place a staged grow can be noticed to have landed.
        FWriteScopeLock Lock(Mutex);
        PublishPendingLocked();

        // The published slot count rather than the assigned one, since a stale index reads its zeroed header.
        FMaterialTableGPU Table;
        Table.Headers.Address = Headers.Live.Gpu;
        Table.Headers.Count   = PublishedSlots();
        Table.Data.Address    = Pool.Live.Gpu;
        Table.Data.Count      = PublishedPoolUnits() * 4u;
        return Table;
    }

    uint32 FMaterialManager::GetCapacity() const
    {
        FReadScopeLock Lock(Mutex);
        return PublishedSlots();
    }

    uint32 FMaterialManager::GetNumMaterials() const
    {
        FReadScopeLock Lock(Mutex);
        return NumMaterials;
    }

    uint32 FMaterialManager::GetSlotFlags(uint32 Index) const
    {
        FReadScopeLock Lock(Mutex);
        return Index < (uint32)Mirror.size() ? Mirror[Index].Flags : 0u;
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

        const FMaterialUniforms& Uniforms = Mirror[Index];

        uint32 Count = 0;
        for (uint32 i = 0; i < MAX_TEXTURES && Count < MaxIDs; ++i)
        {
            const uint32 ID = Uniforms.Textures[i];
            if (ID == 0 || ID == kInvalidHeapSlot)
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
        Headers.Publish();
        Pool.Publish();
    }

    void FMaterialManager::StageGrowLocked(uint32 MinSlots)
    {
        const uint32 Capacity = PublishedSlots();
        if (Headers.IsStaging() || Capacity >= kMaxMaterialSlots)
        {
            return;
        }

        const uint32 Target      = Math::Max(Math::Max(Capacity * 2, kInitialMaterialSlots), MinSlots);
        const uint32 NewCapacity = Math::Min(Target, kMaxMaterialSlots);
        if (NewCapacity <= Capacity)
        {
            return;
        }

        // Sized with the staged table so a published slot always has a mirror entry.
        Mirror.resize(NewCapacity);
        HeaderImage.resize(NewCapacity);
        BlockUnits.resize(NewCapacity);
        Headers.Stage(HeaderImage.data(), sizeof(FMaterialHeaderGPU) * NewCapacity);
    }

    bool FMaterialManager::GrowLocked(uint32 MinSlots)
    {
        PublishPendingLocked();
        if (MinSlots <= PublishedSlots())
        {
            return true;
        }

        if (PublishedSlots() >= kMaxMaterialSlots)
        {
            return false;
        }

        if (Headers.PendingBytes < (uint64)MinSlots * sizeof(FMaterialHeaderGPU))
        {
            Headers.DropPending();
        }

        StageGrowLocked(MinSlots);

        // The slots are needed now, so this is the one path that waits. AddMaterial stages early to keep it rare.
        if (Headers.IsStaging())
        {
            FlushUploadsAndWait();
            PublishPendingLocked();
        }

        return MinSlots <= PublishedSlots();
    }

    void FMaterialManager::StagePoolGrowLocked(uint32 MinUnits)
    {
        const uint32 Units = PublishedPoolUnits();
        if (Pool.IsStaging())
        {
            return;
        }

        const uint32 MaxUnits = kMaxMaterialSlots * kMaxBlockUnits;
        const uint32 NewUnits = Math::Min(Math::Max(Math::Max(Units * 2, kInitialPoolUnits), MinUnits), MaxUnits);
        if (NewUnits <= Units)
        {
            return;
        }

        PoolImage.resize((size_t)NewUnits * 4u);
        Pool.Stage(PoolImage.data(), (uint64)NewUnits * kPoolUnitBytes);
    }

    bool FMaterialManager::GrowPoolLocked(uint32 MinUnits)
    {
        PublishPendingLocked();
        if (MinUnits <= PublishedPoolUnits())
        {
            return true;
        }

        if (Pool.PendingBytes < (uint64)MinUnits * kPoolUnitBytes)
        {
            Pool.DropPending();
        }

        StagePoolGrowLocked(MinUnits);

        if (Pool.IsStaging())
        {
            FlushUploadsAndWait();
            PublishPendingLocked();
        }

        return MinUnits <= PublishedPoolUnits();
    }

    uint32 FMaterialManager::AllocateBlockLocked(uint32 Units)
    {
        if (!FreeBlocks[Units].empty())
        {
            const uint32 Reused = FreeBlocks[Units].back();
            FreeBlocks[Units].pop_back();
            return Reused;
        }

        if (PoolTop + Units > PublishedPoolUnits() && !GrowPoolLocked(PoolTop + Units))
        {
            return Constants::kIndexNoneU32;
        }

        const uint32 Offset = PoolTop;
        PoolTop += Units;

        if (PoolTop + kPoolGrowSlack >= PublishedPoolUnits())
        {
            StagePoolGrowLocked(PoolTop + kPoolGrowSlack + 1u);
        }
        return Offset;
    }

    void FMaterialManager::FreeBlockLocked(uint32 Index)
    {
        if (BlockUnits[Index] != 0)
        {
            FreeBlocks[BlockUnits[Index]].push_back(HeaderImage[Index].DataWord / 4u);
            BlockUnits[Index] = 0;
        }
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
            if (HighWater == PublishedSlots() && !GrowLocked(PublishedSlots() + 1))
            {
                LOG_ERROR("MaterialManager: material table is full at {} slots; '{}' will not render "
                    "with its own parameters.", PublishedSlots(), Material->GetName());
                return;
            }

            FreeIndex = HighWater++;
        }

        // Staged before the wall, so the copy has frames to land in and the grow above rarely waits.
        PublishPendingLocked();
        if (HighWater + kGrowSlack >= PublishedSlots())
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
        if (Index < PublishedSlots())
        {
            FreeBlockLocked(Index);
        }

        FreeList.push_back(Index);
        --NumMaterials;
    }

    void FMaterialManager::UpdateMaterialUniforms(const FMaterialUniforms* InUniforms, uint32 Index)
    {
        FWriteScopeLock Lock(Mutex);

        WriteSlotLocked(InUniforms, Index);
    }

    void FMaterialManager::UpdateMaterialUniformRange(uint32 Index, uint32 ByteOffset, const void* Data, uint32 ByteSize)
    {
        if (Data == nullptr || ByteSize == 0 || (uint64)ByteOffset + ByteSize > sizeof(FMaterialUniforms))
        {
            return;
        }

        FWriteScopeLock Lock(Mutex);

        if (Index >= PublishedSlots())
        {
            return;
        }

        std::byte* Slot = reinterpret_cast<std::byte*>(&Mirror[Index]) + ByteOffset;
        if (Memory::MemEqual(Slot, Data, ByteSize))
        {
            return;
        }

        Memory::Memcpy(Slot, Data, ByteSize);
        PackSlotLocked(Index, ByteOffset, ByteOffset + ByteSize);
    }

    void FMaterialManager::WriteSlotLocked(const FMaterialUniforms* InUniforms, uint32 Index)
    {
        if (Index >= PublishedSlots())
        {
            return;
        }

        // Zeroed slot doubles as the removed state.
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
        PackSlotLocked(Index, 0, sizeof(FMaterialUniforms));
    }

    void FMaterialManager::PackSlotLocked(uint32 Index, uint32 DirtyBegin, uint32 DirtyEnd)
    {
        const FMaterialUniforms&  U   = Mirror[Index];
        const FMaterialHeaderGPU  Old = HeaderImage[Index];

        FPackedLayout Layout = MeasureLayout(U);
        uint32 DataWord      = Old.DataWord;

        // A changed layout shifts every section after the one that changed, so the whole block goes up.
        if (!(Layout == UnpackLayout(Old.Layout)))
        {
            DirtyBegin = 0;
            DirtyEnd   = sizeof(FMaterialUniforms);
        }

        if (Layout.Units() > BlockUnits[Index])
        {
            FreeBlockLocked(Index);
            const uint32 Offset = AllocateBlockLocked(Layout.Units());
            if (Offset == Constants::kIndexNoneU32)
            {
                LOG_ERROR("MaterialManager: the material data pool is full, so slot {} reads zero parameters.", Index);
                Layout   = {};
                DataWord = 0;
            }
            else
            {
                BlockUnits[Index] = (uint8)Layout.Units();
                DataWord          = Offset * 4u;
            }
        }

        auto PackSection = [&](uint32 SourceOffset, uint32 UsedBytes, uint32 DestWord)
        {
            const uint32 Begin = Math::Max(DirtyBegin, SourceOffset);
            const uint32 End   = Math::Min(DirtyEnd, SourceOffset + UsedBytes);
            if (Begin >= End)
            {
                return;
            }

            const uint32 FirstWord = (Begin - SourceOffset) / 4u;
            const uint32 EndWord   = (End - SourceOffset + 3u) / 4u;
            const uint32 Bytes     = (EndWord - FirstWord) * 4u;
            uint32* Dest = &PoolImage[DestWord + FirstWord];
            Memory::Memcpy(Dest, reinterpret_cast<const std::byte*>(&U) + SourceOffset + FirstWord * 4u, Bytes);
            Pool.Write((uint64)(DestWord + FirstWord) * 4u, Dest, Bytes);
        };

        const uint32 ScalarWord  = DataWord + Layout.NumVectors * 4u;
        const uint32 TextureWord = ScalarWord + Layout.NumScalars;
        PackSection(offsetof(FMaterialUniforms, Vectors),  Layout.NumVectors * 16u, DataWord);
        PackSection(offsetof(FMaterialUniforms, Scalars),  Layout.NumScalars * 4u,  ScalarWord);
        PackSection(offsetof(FMaterialUniforms, Textures), Layout.NumTextures * 4u, TextureWord);

        const FMaterialHeaderGPU Header = MakeHeader(U, Layout, DataWord);
        if (!Memory::MemEqual(&Header, &Old))
        {
            HeaderImage[Index] = Header;
            Headers.Write((uint64)Index * sizeof(FMaterialHeaderGPU), &HeaderImage[Index], sizeof(FMaterialHeaderGPU));
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
            return Constants::kIndexNone;
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
