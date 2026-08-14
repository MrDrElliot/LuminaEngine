#pragma once

#include "RHI.h"

namespace Lumina::RHI
{
    RUNTIME_API void UploadBuffer(GPUPtr Dest, const void* Data, uint64 Size);

    // Width/Height are the mip's own dimensions, REQUIRED past mip 0 and whenever OffsetY is set. OffsetY
    // stages a horizontal band. False = the upload was DROPPED, so a banded caller must not advance.
    RUNTIME_API bool UploadTexture(FTextureH Dest, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels = 0, uint32 Width = 0, uint32 Height = 0, uint32 OffsetY = 0);

    /** Enqueue a GPU-side mip copy between two images, in the same flush (and the same barriers) as the
     *  host uploads around it. No staging, no host bandwidth: this is what a mip that ALREADY exists on
     *  the GPU should cost when it moves between images, which is most of a texture residency change. */
    RUNTIME_API void UploadTextureCopy(FTextureH Dest, uint32 DestLayer, uint32 DestMip,
                                       FTextureH Source, uint32 SourceLayer, uint32 SourceMip,
                                       uint32 Width, uint32 Height);

    // Enqueue a full-texture clear to an RGBA float value (no staging). Thread-safe.
    RUNTIME_API void UploadClearTexture(FTextureH Dest, const float Value[4]);

    RUNTIME_API void FlushUploadsAndWait();

    namespace Upload
    {
        void Initialize();
        void Shutdown();

        // Both hand the staging blocks back rather than releasing them: only the CALLER knows when the copies
        // retire. Releasing inside the flush retires against the PREVIOUS slot during BeginFrame.
        //
        // OutBatch names the flush the swept-out ops went into, for NoteFlushSubmitted and IsBatchComplete.
        bool Flush(FCmdListH CL, TVector<GPUPtr>& OutOwnedStaging, uint32* OutSliceMask = nullptr, uint64* OutBatch = nullptr);

        uint32 FlushSplit(FCmdListH BufferCL, FCmdListH ImageCL,
                          uint32* OutBufferSliceMask, uint32* OutImageSliceMask,
                          TVector<GPUPtr>& OutOwnedStaging, uint64* OutBatch = nullptr);

        /** The flush that ops queued right now will leave in. Read it AFTER queueing the ops you care
         *  about: the answer is then either the batch they are in, or -- if a flush raced in between -- a
         *  later one, and waiting for a later batch is conservative rather than wrong.
         *
         *  This is the only honest answer to "are my bytes on the GPU yet". A frame count is not: uploads
         *  flush at the top of BeginFrame, so what a queue-time frame number means depends on where in the
         *  frame the caller stood. */
        uint64 BatchForQueuedOps();

        /** True once that batch has been submitted AND its submission has completed on every queue it
         *  touched -- i.e. the copies have actually run and the destination holds the data. */
        bool IsBatchComplete(uint64 Batch);

        // Drop every queued op that would write to this resource, and MUST be called before the resource is
        // released. Core::BeginFrame drains the retire queue BEFORE it flushes, so an op left pointing at a
        // released resource records against a destroyed VkImage / freed address -- and once the handle or the
        // address is recycled, against a different LIVE one, which corrupts silently instead of faulting.
        // Both are cheap no-ops while nothing is queued, which is the common case on the release paths.
        void CancelTexture(FTextureH Texture);
        void CancelBuffer(GPUPtr Dest);

        void DrainSliceWriters(uint32 Slot);

        // Records the fence the flush signals: per staging slice (so BeginSlot knows when the slice is
        // reusable) and per batch (so IsBatchComplete knows when the copies have actually run).
        void NoteFlushSubmitted(uint64 Batch, uint32 SliceMask, EQueueType Queue, FSemaphoreH Semaphore, uint64 Value);

        void BeginSlot(uint32 Slot);
    }
}
