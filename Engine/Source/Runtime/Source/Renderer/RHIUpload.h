#pragma once

#include "RHI.h"

namespace Lumina::RHI
{
    RUNTIME_API void UploadBuffer(GPUPtr Dest, const void* Data, uint64 Size);

    RUNTIME_API void UploadTexture(FTextureH Dest, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels = 0);

    // Enqueue a full-texture clear to an RGBA float value (no staging). Thread-safe.
    RUNTIME_API void UploadClearTexture(FTextureH Dest, const float Value[4]);

    RUNTIME_API void FlushUploadsAndWait();

    namespace Upload
    {
        void Initialize();
        void Shutdown();

        bool Flush(FCmdListH CL, uint32* OutSliceMask = nullptr);

        uint32 FlushSplit(FCmdListH BufferCL, FCmdListH ImageCL,
                          uint32* OutBufferSliceMask, uint32* OutImageSliceMask);

        void DrainSliceWriters(uint32 Slot);

        void NoteFlushSubmitted(uint32 SliceMask, EQueueType Queue, FSemaphoreH Semaphore, uint64 Value);

        void BeginSlot(uint32 Slot);
    }
}
