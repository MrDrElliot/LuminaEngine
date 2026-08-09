#pragma once

#include "RHI.h"

namespace Lumina::RHI
{
    RUNTIME_API void UploadBuffer(GPUPtr Dest, const void* Data, uint64 Size);

    // Width/Height are the mip's own dimensions; 0 derives them from the texture description, which is
    // only correct for mip 0. See RHITexture.h.
    RUNTIME_API void UploadTexture(FTextureH Dest, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels = 0, uint32 Width = 0, uint32 Height = 0);

    // Enqueue a full-texture clear to an RGBA float value (no staging). Thread-safe.
    RUNTIME_API void UploadClearTexture(FTextureH Dest, const float Value[4]);

    RUNTIME_API void FlushUploadsAndWait();

    namespace Upload
    {
        void Initialize();
        void Shutdown();

        // Both hand back the dedicated staging blocks the flushed ops own instead of releasing them, because
        // only the CALLER knows when the copies reading them retire. Releasing inside the flush retires
        // against whatever slot is current at RECORD time, which during BeginFrame is still the PREVIOUS
        // slot -- so the block was freed after waiting a timeline value older than the submit that reads it.
        // On the async transfer queue that window is wide enough to hand the copy engine a freed address.
        bool Flush(FCmdListH CL, TVector<GPUPtr>& OutOwnedStaging, uint32* OutSliceMask = nullptr);

        uint32 FlushSplit(FCmdListH BufferCL, FCmdListH ImageCL,
                          uint32* OutBufferSliceMask, uint32* OutImageSliceMask,
                          TVector<GPUPtr>& OutOwnedStaging);

        void DrainSliceWriters(uint32 Slot);

        void NoteFlushSubmitted(uint32 SliceMask, EQueueType Queue, FSemaphoreH Semaphore, uint64 Value);

        void BeginSlot(uint32 Slot);
    }
}
