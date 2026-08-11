#pragma once

#include "MaterialTypes.h"
#include "RHI.h"
#include "Containers/Array.h"

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

        RUNTIME_API void UpdateMaterialUniforms(const FMaterialUniforms* InUniforms, uint32 Index);

        /** Writes ByteSize bytes at ByteOffset inside slot Index and uploads only that sub-range.
            For a single changed parameter, which is a 4 or 16 byte write into a 592 byte block --
            pushing the whole block would stage 37x the bytes to land one float. */
        RUNTIME_API void UpdateMaterialUniformRange(uint32 Index, uint32 ByteOffset, const void* Data, uint32 ByteSize);

        RUNTIME_API GPUPtr GetMaterialBuffer() const;

        /** Slots the table can currently hold; grows by doubling. */
        RUNTIME_API uint32 GetCapacity() const;

        /** Slots currently assigned to a material. */
        RUNTIME_API uint32 GetNumMaterials() const;

    private:

        /** Copies the mirror into a buffer of at least MinSlots, retires the old one and publishes the
            new address. Returns false only at the slot ceiling. Caller holds the write lock. */
        bool GrowLocked(uint32 MinSlots);

        /** Writes one slot to the mirror and uploads it. Null uniforms zero the slot. Caller holds
            either lock -- distinct slots are written independently, and only a grow (write lock)
            invalidates the mirror's storage or the buffer address. */
        void WriteSlotLocked(const FMaterialUniforms* InUniforms, uint32 Index);

        mutable FSharedMutex                    Mutex;

        /** Returned slots, reused before the high-water mark advances so the table stays dense. */
        TVector<uint32>                         FreeList;

        /** CPU copy of the whole table. Exists so a grow can repopulate the new buffer without a
            GPU-side copy and its barriers; also what keeps freed slots reading as zero. */
        TVector<FMaterialUniforms>              Mirror;

        GPUPtr                                  MaterialBuffer = 0;
        uint32                                  Capacity = 0;

        /** Highest slot ever handed out, +1. Slots below it are either live or in FreeList. */
        uint32                                  HighWater = 0;
        uint32                                  NumMaterials = 0;
    };
}
