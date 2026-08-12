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

        // Both hand the staging blocks back rather than releasing them: only the CALLER knows when the copies
        // retire. Releasing inside the flush retires against the PREVIOUS slot during BeginFrame.
        bool Flush(FCmdListH CL, TVector<GPUPtr>& OutOwnedStaging, uint32* OutSliceMask = nullptr);

        uint32 FlushSplit(FCmdListH BufferCL, FCmdListH ImageCL,
                          uint32* OutBufferSliceMask, uint32* OutImageSliceMask,
                          TVector<GPUPtr>& OutOwnedStaging);

        // Drop every queued op that would write to this resource, and MUST be called before the resource is
        // released. Core::BeginFrame drains the retire queue BEFORE it flushes, so an op left pointing at a
        // released resource records against a destroyed VkImage / freed address -- and once the handle or the
        // address is recycled, against a different LIVE one, which corrupts silently instead of faulting.
        // Both are cheap no-ops while nothing is queued, which is the common case on the release paths.
        void CancelTexture(FTextureH Texture);
        void CancelBuffer(GPUPtr Dest);

        void DrainSliceWriters(uint32 Slot);

        void NoteFlushSubmitted(uint32 SliceMask, EQueueType Queue, FSemaphoreH Semaphore, uint64 Value);

        void BeginSlot(uint32 Slot);
    }
}
