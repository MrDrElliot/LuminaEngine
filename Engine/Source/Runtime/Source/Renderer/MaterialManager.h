#pragma once

#include "MaterialTypes.h"
#include "RHI.h"
#include "GPUSpan.h"
#include "Containers/Vector.h"

namespace Lumina
{
    class CMaterialInterface;
    struct FMaterialUniforms;
    struct FMaterialCollectionUniforms;
}

namespace Lumina::RHI
{
    // A GPU buffer kept equal to a CPU image, so a grow is a staged verbatim copy and every offset survives it.
    struct FMirroredGPUBuffer
    {
        FGPUAllocation  Live = {};
        uint64          LiveBytes = 0;

        // A grown copy still queued; publishing before it runs would read recycled VRAM.
        FGPUAllocation  Pending = {};
        uint64          PendingBytes = 0;
        uint64          PendingBatch = 0;

        bool IsStaging() const { return Pending.Gpu != 0; }
        void Write(uint64 Offset, const void* Data, uint64 Size);
        bool Stage(const void* Image, uint64 Bytes);
        void Publish();
        void DropPending();
        void Release();
    };

    /**
     * Owns the GPU material table and hands out the slot index each CMaterialInterface carries.
     */
    class FMaterialManager final
    {
    public:

        FMaterialManager();
        ~FMaterialManager();
        LE_NO_COPYMOVE(FMaterialManager);

        RUNTIME_API void AddMaterial(CMaterialInterface* Material);
        RUNTIME_API void RemoveMaterial(CMaterialInterface* Material);

        // Frees a slot by index for the deferred release path; never call it while the owner still names the slot.
        RUNTIME_API void RemoveMaterialSlot(uint32 Index);

        RUNTIME_API void UpdateMaterialUniforms(const FMaterialUniforms* InUniforms, uint32 Index);

        // ByteOffset is into FMaterialUniforms; only the packed words that range lands on are uploaded.
        RUNTIME_API void UpdateMaterialUniformRange(uint32 Index, uint32 ByteOffset, const void* Data, uint32 ByteSize);

        // What consumers bind. Also the per-frame publish point for a staged grow, which is why it is not const.
        RUNTIME_API FMaterialTableGPU GetMaterialTable();

        RUNTIME_API uint32 GetCapacity() const;
        RUNTIME_API uint32 GetNumMaterials() const;

        // The distinct bindless texture IDs slot Index samples, read from the same mirror the GPU blocks are packed from.
        RUNTIME_API uint32 CopySlotTextureIDs(uint32 Index, uint32* OutIDs, uint32 MaxIDs) const;

        RUNTIME_API uint32 GetSlotFlags(uint32 Index) const;

    private:

        uint32 PublishedSlots() const { return (uint32)(Headers.LiveBytes / sizeof(FMaterialHeaderGPU)); }
        uint32 PublishedPoolUnits() const { return (uint32)(Pool.LiveBytes / kPoolUnitBytes); }

        void PublishPendingLocked();
        bool GrowLocked(uint32 MinSlots);
        void StageGrowLocked(uint32 MinSlots);
        bool GrowPoolLocked(uint32 MinUnits);
        void StagePoolGrowLocked(uint32 MinUnits);

        uint32 AllocateBlockLocked(uint32 Units);
        void   FreeBlockLocked(uint32 Index);

        // Repacks the slot from Mirror, uploading only what the dirty FMaterialUniforms byte range reaches.
        void PackSlotLocked(uint32 Index, uint32 DirtyBegin, uint32 DirtyEnd);
        void WriteSlotLocked(const FMaterialUniforms* InUniforms, uint32 Index);

        static constexpr uint32 kPoolUnitBytes = 16;
        static constexpr uint32 kMaxBlockUnits = (MAX_VECTORS * 16 + MAX_SCALARS * 4 + MAX_TEXTURES * 4 + kPoolUnitBytes - 1) / kPoolUnitBytes;

        mutable FSharedMutex                    Mutex;

        TVector<uint32>                         FreeList;

        // What each slot's block is packed from, and what freed slots read back as zero from.
        TVector<FMaterialUniforms>              Mirror;
        TVector<FMaterialHeaderGPU>             HeaderImage;
        TVector<uint8>                          BlockUnits;
        TVector<uint32>                         PoolImage;

        // Freed blocks by size; materials sharing a master share a block size, so exact reuse is the common case.
        TVector<uint32>                         FreeBlocks[kMaxBlockUnits + 1];
        uint32                                  PoolTop = 0;

        FMirroredGPUBuffer                      Headers;
        FMirroredGPUBuffer                      Pool;

        uint32                                  HighWater = 0;
        uint32                                  NumMaterials = 0;
    };

    /** The GPU table of parameter collections, capped and allocated once, with slot 0 reserved as zero. */
    class FMaterialCollectionManager final
    {
    public:

        FMaterialCollectionManager();
        ~FMaterialCollectionManager();
        LE_NO_COPYMOVE(FMaterialCollectionManager);

        /** A free slot, or Constants::kIndexNone when the table is full. The slot reads zero until Update. */
        RUNTIME_API int32 Acquire();

        RUNTIME_API void Release(int32 Index);

        RUNTIME_API void Update(int32 Index, const FMaterialCollectionUniforms& Uniforms);

        /** Writes ByteSize bytes at ByteOffset inside slot Index, for a single changed parameter. */
        RUNTIME_API void UpdateRange(int32 Index, uint32 ByteOffset, const void* Data, uint32 ByteSize);

        RUNTIME_API GPUPtr GetBuffer() const { return CollectionBuffer.Gpu; }

        TGPUSpan<FMaterialCollectionUniforms> GetSpan() const { return { CollectionBuffer }; }

    private:

        mutable FSharedMutex                        Mutex;
        TVector<int32>                              FreeList;
        TVector<FMaterialCollectionUniforms>        Mirror;
        FGPUAllocation                              CollectionBuffer = {};

        /** Starts at 1, since slot 0 is the reserved zero collection. */
        int32                                       HighWater = 1;
    };
}
