#pragma once

#include "MaterialTypes.h"
#include "RHI.h"
#include "Containers/Vector.h"

namespace Lumina
{
    class CMaterialInterface;
    struct FMaterialUniforms;
}

namespace Lumina::RHI
{
    /**
     * Owns the GPU material uniform table and hands out the slot index each CMaterialInterface carries.
     */
    class FMaterialManager final
    {
    public:

        FMaterialManager();
        ~FMaterialManager();
        LE_NO_COPYMOVE(FMaterialManager);

        RUNTIME_API void AddMaterial(CMaterialInterface* Material);
        RUNTIME_API void RemoveMaterial(CMaterialInterface* Material);

        /** Zeroes and frees a slot by index, with no reference to the material that held it.
            This is the form the deferred release path uses: by the time it runs the owner is gone, and
            the renderer must not need it back. RemoveMaterial is now a thin wrapper that additionally
            clears the owner's cached index -- do NOT call this one while the owner is still alive, or it
            will keep handing out a slot that has been returned to the free list. */
        RUNTIME_API void RemoveMaterialSlot(uint32 Index);

        RUNTIME_API void UpdateMaterialUniforms(const FMaterialUniforms* InUniforms, uint32 Index);

        /** Writes ByteSize bytes at ByteOffset inside slot Index and uploads only that sub-range.
            For a single changed parameter, which is a 4 or 16 byte write into a 592 byte block --
            pushing the whole block would stage 37x the bytes to land one float. */
        RUNTIME_API void UpdateMaterialUniformRange(uint32 Index, uint32 ByteOffset, const void* Data, uint32 ByteSize);

        // Also the per-frame publish point for a staged table, which is why it is not const.
        RUNTIME_API GPUPtr GetMaterialBuffer();

        /** Slots the table can currently hold; grows by doubling. */
        RUNTIME_API uint32 GetCapacity() const;

        /** Slots currently assigned to a material. */
        RUNTIME_API uint32 GetNumMaterials() const;

        /** The distinct bindless texture IDs slot Index samples, written into OutIDs; returns how many.
         *  Read straight out of the uniform mirror -- the same bytes the shader reads -- so it cannot
         *  disagree with what the material actually samples. This is what turns the GPU's per-material
         *  streaming feedback into per-texture residency demand. */
        RUNTIME_API uint32 CopySlotTextureIDs(uint32 Index, uint32* OutIDs, uint32 MaxIDs) const;

    private:

        // Ensures the PUBLISHED table holds at least MinSlots. Caller holds the write lock.
        bool GrowLocked(uint32 MinSlots);

        // Queues the mirror into a table of at least MinSlots, publishing nothing. Write lock held.
        void StageGrowLocked(uint32 MinSlots);

        // Swaps the staged table in once its mirror copy has actually run. Caller holds the write lock.
        void PublishPendingLocked();

        // Writes one slot to the mirror and uploads it. Null uniforms zero the slot. Either lock: only a grow moves the mirror's storage.
        void WriteSlotLocked(const FMaterialUniforms* InUniforms, uint32 Index);

        mutable FSharedMutex                    Mutex;

        /** Returned slots, reused before the high-water mark advances so the table stays dense. */
        TVector<uint32>                         FreeList;

        /** CPU copy of the whole table. Exists so a grow can repopulate the new buffer without a
            GPU-side copy and its barriers; also what keeps freed slots reading as zero. */
        TVector<FMaterialUniforms>              Mirror;

        GPUPtr                                  MaterialBuffer = 0;
        uint32                                  Capacity = 0;

        // A grown table whose mirror copy is still queued; publishing before it runs would read recycled VRAM as uniforms.
        GPUPtr                                  PendingBuffer = 0;
        uint32                                  PendingCapacity = 0;
        uint64                                  PendingBatch = 0;

        /** Highest slot ever handed out, +1. Slots below it are either live or in FreeList. */
        uint32                                  HighWater = 0;
        uint32                                  NumMaterials = 0;
    };
}
