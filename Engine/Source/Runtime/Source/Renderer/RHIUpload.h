#pragma once

#include "RHI.h"

// Batched GPU upload path (No-Graphics-API style). Callers stage data with
// UploadBuffer / UploadTexture and return immediately; the staged bytes live in a
// per-frame CPU-write linear ring and the copy is recorded once at the next
// RHI::Core::BeginFrame (Upload::Flush) followed by a single Transfer->All barrier.
//
// This replaces the old per-call "Malloc staging -> submit -> block on a fence ->
// Free" pattern: no stall, one staging allocator, one barrier per frame.
//
// Readiness: an upload becomes resident at the next BeginFrame flush. Data that must
// be resident before its first use (boot placeholders, stock LUTs) uses
// FlushUploadsAndWait, which preserves the old synchronous guarantee.

namespace Lumina::RHI
{
    // Stage Size bytes from Data and enqueue a copy to the buffer at Dest. If Dest is
    // host-visible the bytes are written through immediately (no copy queued).
    // Thread-safe. Does nothing on (0 / null / 0-size).
    RUNTIME_API void UploadBuffer(GPUPtr Dest, const void* Data, uint64 Size);

    // Stage one mip's tightly-packed pixels and enqueue a memory->texture copy.
    // RowPitchTexels 0 = mip width. Thread-safe.
    RUNTIME_API void UploadTexture(FTextureH Dest, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels = 0);

    // Enqueue a full-texture clear to an RGBA float value (no staging). Thread-safe.
    RUNTIME_API void UploadClearTexture(FTextureH Dest, const float Value[4]);

    // Flush everything queued so far on a one-off command list and block until the GPU
    // finishes. For boot/stock data that must be resident before first use.
    RUNTIME_API void FlushUploadsAndWait();

    namespace Upload
    {
        void Initialize();
        void Shutdown();

        // Render thread, from RHI::Core::BeginFrame. Records every queued copy + one
        // Transfer->All barrier into CL; returns false (and records nothing) when the
        // queue is empty so the caller can skip the submit.
        bool Flush(FCmdListH CL);

        // Render thread, from RHI::Core::BeginFrame after advancing the frame slot.
        // Recycles the slot's staging slice for fresh writes.
        void BeginSlot(uint32 Slot);
    }
}
