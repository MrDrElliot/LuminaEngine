#pragma once

#include "RHI.h"

namespace Lumina::RHI
{
    // False means the copy was NOT queued (staging exhausted), so no batch will ever name it.
    RUNTIME_API bool UploadBuffer(const FGPUAllocation& Dest, const void* Data, uint64 Size, uint64 Offset = 0);

    // Pass the mip's own size past mip 0 or with OffsetY, and a banded caller must not advance when this returns false.
    RUNTIME_API bool UploadTexture(FTextureH Dest, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels = 0, uint32 Width = 0, uint32 Height = 0, uint32 OffsetY = 0);

    // A GPU-side mip copy with no staging, riding the same flush and barriers as the host uploads around it.
    RUNTIME_API void UploadTextureCopy(FTextureH Dest, uint32 DestLayer, uint32 DestMip,
                                       FTextureH Source, uint32 SourceLayer, uint32 SourceMip,
                                       uint32 Width, uint32 Height);

    // Enqueue a full-texture clear to an RGBA float value (no staging). Thread-safe.
    RUNTIME_API void UploadClearTexture(FTextureH Dest, const float Value[4]);

    RUNTIME_API void FlushUploadsAndWait();

    // Submits every queued upload now on the graphics queue and returns that timeline value, 0 when nothing was queued.
    RUNTIME_API uint64 FlushUploads();

    namespace Upload
    {
        void Initialize();
        void Shutdown();

        // Staging goes back to the caller, because releasing inside the flush would retire against the previous slot.
        bool Flush(FCmdListH CL, TVector<FGPUAllocation>& OutOwnedStaging, uint64* OutBatch = nullptr);

        uint32 FlushSplit(FCmdListH BufferCL, FCmdListH ImageCL, TVector<FGPUAllocation>& OutOwnedStaging, uint64* OutBatch = nullptr);

        // Read after queueing the ops you care about, so the answer is their batch or a later one.
        uint64 BatchForQueuedOps();

        // True once the batch has completed on every queue it touched, so the destination holds the data.
        bool IsBatchComplete(uint64 Batch);

        // Call before releasing the resource, since BeginFrame retires before it flushes and a stale op corrupts whatever reuses it.
        void CancelTexture(FTextureH Texture);
        void CancelBuffer(const FGPUAllocation& Dest);

        // Records the value the flush signals, which frees its staging and tells IsBatchComplete the copies ran.
        void NoteFlushSubmitted(uint64 Batch, EQueueType Queue, FSemaphoreH Semaphore, uint64 Value);

        RUNTIME_API void LogStats(FStringView Label);
        void PublishProfileCounters();
    }
}
