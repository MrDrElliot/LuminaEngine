#include "RuntimePCH.h"
#include "RHIUpload.h"
#include "RHICore.h"

#include "Core/Math/Math.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "Memory/Memcpy.h"

namespace Lumina::RHI
{
    namespace
    {
        // Per-frame-in-flight staging slice. Sized for a steady-state burst; an upload
        // larger than the slice's remaining space falls back to a dedicated (deferred-
        // freed) allocation, so this never has to fit a worst-case load spike.
        constexpr uint64 kStagingSliceSize = 64ull * 1024 * 1024;

        // Staging that came from a dedicated allocation rather than one of the slices.
        constexpr uint8 kNoSlice = 0xFF;

        enum class EUploadOp : uint8 { Buffer, Texture, Clear };

        struct FUploadOp
        {
            EUploadOp   Type;
            GPUPtr      Staging        = 0;         // source for Buffer/Texture, 0 for Clear
            bool        bOwnedStaging  = false;     // true -> DeferredFree after the copy retires
            uint8       Slice          = kNoSlice;  // slice the staging was reserved from
            GPUPtr      BufferDest     = 0;         // Buffer
            FTextureH   TextureDest    = {};        // Texture/Clear
            uint64      Size           = 0;
            uint32      RowPitchTexels = 0;         // Texture
            uint32      Mip            = 0;         // Texture
            uint32      Layer          = 0;         // Texture (array slice; 0 for non-array)
            float       ClearValue[4]  = {};        // Clear
        };

        struct FStagingSlice
        {
            GPUPtr      Gpu    = 0;
            std::byte*  Cpu    = nullptr;
            uint64      Cursor = 0;

            // Reservations that have not yet finished their memcpy and queued their op. Callers copy
            // outside the lock, so the cursor alone says nothing about whether a slice is quiet: a
            // worker that reserved before a reset can still be writing bytes a later reserve has
            // already handed out, and an op queued after a Flush would point into recycled staging.
            TAtomic<uint32> Writers{0};

            // Flush submission that last recorded copies out of this slice. The frame ring waits for
            // the frame that WROTE a slice, but the flush that READS it is submitted a frame later
            // and carries a higher timeline value, so recycling has to gate on this separately.
            FSemaphoreH ReadSemaphore = {};
            uint64      ReadValue     = 0;
        };

        struct FUploadState
        {
            FStagingSlice       Slices[kFramesInFlight];
            uint32              CurrentSlot = 0;
            TVector<FUploadOp>  Queue;
            FMutex              Mutex;

            FSemaphoreH         FlushSemaphore;
            uint64              FlushCounter = 0;

            bool                bInitialized = false;
        };

        FUploadState GUpload;

        struct FStaging
        {
            std::byte*  Cpu    = nullptr;
            GPUPtr      Gpu    = 0;
            bool        bOwned = false;
            uint8       Slice  = kNoSlice;
        };

        // Reserve Size bytes of CPU-write staging from the current slice; falls back to a
        // dedicated allocation when the slice is full. Caller memcpys outside the lock and must
        // pair this with EndWrite once the bytes are in AND the op is queued. A null Cpu means
        // the fallback allocation failed and there is nothing to stage into.
        FStaging ReserveLocked(uint64 Size, uint64 Alignment)
        {
            const uint32 Slot = GUpload.CurrentSlot;
            FStagingSlice& Slice = GUpload.Slices[Slot];
            const uint64 Aligned = Math::AlignUp(Slice.Cursor, Alignment);
            if (Aligned + Size <= kStagingSliceSize)
            {
                Slice.Cursor = Aligned + Size;
                Slice.Writers.fetch_add(1, std::memory_order_relaxed);
                return { Slice.Cpu + Aligned, Slice.Gpu + Aligned, false, (uint8)Slot };
            }

            const GPUPtr Owned = Malloc(Size, Alignment, EMemoryType::CPUWrite);
            if (Owned == 0)
            {
                return {};
            }
            return { static_cast<std::byte*>(ToHost(Owned)), Owned, true, kNoSlice };
        }

        // Release the reservation's pin. Must run after the op is in the queue, so that a drained
        // writer count means every op referencing the slice is visible to the next Flush.
        void EndWrite(const FStaging& Staging)
        {
            if (Staging.Slice != kNoSlice)
            {
                GUpload.Slices[Staging.Slice].Writers.fetch_sub(1, std::memory_order_release);
            }
        }
    }

    void UploadBuffer(GPUPtr Dest, const void* Data, uint64 Size)
    {
        if (Dest == 0 || Data == nullptr || Size == 0)
        {
            return;
        }

        // Host-visible destination: write through the mapping, nothing to stage.
        if (void* Mapped = ToHost(Dest))
        {
            Memory::Memcpy(Mapped, Data, Size);
            return;
        }

        FStaging S;
        {
            FScopeLock Lock(GUpload.Mutex);
            S = ReserveLocked(Size, kDefaultAlign);
        }

        if (S.Cpu == nullptr)
        {
            LOG_ERROR("RHI: dropped a {} KiB buffer upload, staging allocation failed.", Size / 1024);
            return;
        }

        Memory::Memcpy(S.Cpu, Data, Size);

        FUploadOp Op;
        Op.Type          = EUploadOp::Buffer;
        Op.Staging       = S.Gpu;
        Op.bOwnedStaging = S.bOwned;
        Op.Slice         = S.Slice;
        Op.BufferDest    = Dest;
        Op.Size          = Size;

        {
            FScopeLock Lock(GUpload.Mutex);
            GUpload.Queue.push_back(Op);
        }

        EndWrite(S);
    }

    void UploadTexture(FTextureH Dest, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels)
    {
        if (!IsValid(Dest) || Data == nullptr || Size == 0)
        {
            return;
        }

        FStaging S;
        {
            FScopeLock Lock(GUpload.Mutex);
            S = ReserveLocked(Size, kDefaultAlign);
        }

        if (S.Cpu == nullptr)
        {
            LOG_ERROR("RHI: dropped a {} KiB texture upload, staging allocation failed.", Size / 1024);
            return;
        }

        Memory::Memcpy(S.Cpu, Data, Size);

        FUploadOp Op;
        Op.Type           = EUploadOp::Texture;
        Op.Staging        = S.Gpu;
        Op.bOwnedStaging  = S.bOwned;
        Op.Slice          = S.Slice;
        Op.TextureDest    = Dest;
        Op.Size           = Size;
        Op.RowPitchTexels = RowPitchTexels;
        Op.Mip            = Mip;
        Op.Layer          = Layer;

        {
            FScopeLock Lock(GUpload.Mutex);
            GUpload.Queue.push_back(Op);
        }

        EndWrite(S);
    }

    void UploadClearTexture(FTextureH Dest, const float Value[4])
    {
        if (!IsValid(Dest))
        {
            return;
        }

        FUploadOp Op;
        Op.Type        = EUploadOp::Clear;
        Op.TextureDest = Dest;
        Op.ClearValue[0] = Value[0];
        Op.ClearValue[1] = Value[1];
        Op.ClearValue[2] = Value[2];
        Op.ClearValue[3] = Value[3];

        FScopeLock Lock(GUpload.Mutex);
        GUpload.Queue.push_back(Op);
    }

    void FlushUploadsAndWait()
    {
        if (!GUpload.bInitialized)
        {
            return;
        }

        const FCmdListH CL = OpenCommandList(EQueueType::Graphics);
        if (!Upload::Flush(CL))
        {
            ResetCommandList(CL);
            return;
        }

        // Counter is shared with any other thread that reaches this path.
        uint64 Value;
        {
            FScopeLock Lock(GUpload.Mutex);
            Value = ++GUpload.FlushCounter;
        }

        const FSemaphoreInfo Signal { GUpload.FlushSemaphore, Value, EStageFlags::AllCommands };
        Submit(EQueueType::Graphics, TSpan{&CL, 1}, {}, TSpan{&Signal, 1});

        // Deliberately no NoteFlushSubmitted. This path blocks until its own copies retire, so the
        // slices it read need no gate once it returns. Recording one could only do harm: it would
        // overwrite a still-pending gate from the BeginFrame flush with a different semaphore, and
        // (being callable off the main thread) would race BeginSlot's read of those fields.
        WaitSemaphore(GUpload.FlushSemaphore, Value);
        ResetCommandList(CL);
    }

    namespace Upload
    {
        void Initialize()
        {
            for (FStagingSlice& Slice : GUpload.Slices)
            {
                Slice.Gpu    = Malloc(kStagingSliceSize, kDefaultAlign, EMemoryType::CPUWrite);
                Slice.Cpu    = static_cast<std::byte*>(ToHost(Slice.Gpu));
                Slice.Cursor = 0;
                Slice.Writers.store(0, std::memory_order_relaxed);
                Slice.ReadValue = 0;
            }
            GUpload.FlushSemaphore = CreateTimelineSemaphore(0);
            GUpload.CurrentSlot    = 0;
            GUpload.bInitialized   = true;
        }

        void Shutdown()
        {
            if (!GUpload.bInitialized)
            {
                return;
            }

            WaitDeviceIdle();

            // Anything still queued never reached the GPU; free any dedicated staging it owns.
            {
                FScopeLock Lock(GUpload.Mutex);
                for (const FUploadOp& Op : GUpload.Queue)
                {
                    if (Op.bOwnedStaging)
                    {
                        Free(Op.Staging);
                    }
                }
                GUpload.Queue.clear();
            }

            for (FStagingSlice& Slice : GUpload.Slices)
            {
                Free(Slice.Gpu);
                Slice.Gpu    = 0;
                Slice.Cpu    = nullptr;
                Slice.Cursor = 0;
                Slice.Writers.store(0, std::memory_order_relaxed);
                Slice.ReadSemaphore = {};
                Slice.ReadValue     = 0;
            }

            FreeH(GUpload.FlushSemaphore);
            GUpload.bInitialized = false;
        }

        bool Flush(FCmdListH CL, uint32* OutSliceMask)
        {
            TVector<FUploadOp> Ops;
            {
                FScopeLock Lock(GUpload.Mutex);
                if (GUpload.Queue.empty())
                {
                    return false;
                }
                Ops.swap(GUpload.Queue);
            }

            uint32 SliceMask = 0;

            TVector<FTextureH> WrittenTextures;
            auto AlreadyWritten = [&](FTextureH Tex)
            {
                for (const FTextureH& T : WrittenTextures)
                {
                    if (T.Handle == Tex.Handle)
                    {
                        return true;
                    }
                }
                return false;
            };

            // Buffer ranges written since the last transfer barrier, for the same reason the texture
            // list above exists: two copies into one range with nothing between them are unordered,
            // and which one survives is the driver's choice. The queue reaches here holding repeats
            // routinely -- a material slot re-uploaded after a second parameter edit, or freed and
            // immediately reused -- so this is the ordinary case rather than a corner.
            struct FWrittenRange { GPUPtr Begin; GPUPtr End; };
            TVector<FWrittenRange> WrittenBuffers;
            auto OverlapsWritten = [&](GPUPtr Dest, uint64 Size)
            {
                for (const FWrittenRange& Range : WrittenBuffers)
                {
                    if (Dest < Range.End && Range.Begin < Dest + Size)
                    {
                        return true;
                    }
                }
                return false;
            };

            for (const FUploadOp& Op : Ops)
            {
                if (Op.Slice != kNoSlice)
                {
                    SliceMask |= (1u << Op.Slice);
                }

                const bool bWritesTexture = (Op.Type == EUploadOp::Texture || Op.Type == EUploadOp::Clear);
                const bool bWritesBuffer  = (Op.Type == EUploadOp::Buffer);

                // One barrier orders every transfer issued before it, so both records reset together
                // whichever kind tripped it -- otherwise the next repeat of the other kind would
                // insert a second barrier that the first one already covered.
                if ((bWritesTexture && AlreadyWritten(Op.TextureDest))
                 || (bWritesBuffer && OverlapsWritten(Op.BufferDest, Op.Size)))
                {
                    Barriers::TransferToTransfer(CL);
                    WrittenTextures.clear();
                    WrittenBuffers.clear();
                }

                switch (Op.Type)
                {
                case EUploadOp::Buffer:
                    CmdMemcpy(CL, Op.BufferDest, Op.Staging, Op.Size);
                    break;
                case EUploadOp::Texture:
                    {
                        FTextureSlice Slice;
                        Slice.Mip        = Op.Mip;
                        Slice.Layer      = Op.Layer;
                        Slice.LayerCount = 1;
                        CmdCopyMemoryToTexture(CL, Op.Staging, Op.RowPitchTexels, Op.TextureDest, Slice);
                    }
                    break;
                case EUploadOp::Clear:
                    CmdClearTexture(CL, Op.TextureDest, Op.ClearValue);
                    break;
                }

                if (bWritesTexture)
                {
                    WrittenTextures.push_back(Op.TextureDest);
                }
                if (bWritesBuffer)
                {
                    WrittenBuffers.push_back(FWrittenRange{ Op.BufferDest, Op.BufferDest + Op.Size });
                }
            }

            Barriers::TransferToAll(CL);

            // Dedicated (ring-overflow) staging is consumed by the copies above; reclaim it
            // once every in-flight frame has retired.
            for (const FUploadOp& Op : Ops)
            {
                if (Op.bOwnedStaging)
                {
                    Core::DeferredFree(Op.Staging);
                }
            }

            if (OutSliceMask != nullptr)
            {
                *OutSliceMask = SliceMask;
            }
            return true;
        }

        void DrainSliceWriters(uint32 Slot)
        {
            if (!GUpload.bInitialized)
            {
                return;
            }

            // Writers pin whatever CurrentSlot was at reserve time, and CurrentSlot is never this
            // slot when BeginFrame runs, so this only ever spins on a reservation made a full frame
            // cycle ago whose memcpy is still in progress.
            //
            // Read under the lock the reserve took. The pin is incremented there, and without that
            // edge nothing orders the increment against this load: a first read could see a stale
            // zero and let the slice recycle out from under a live writer. Rare enough to never show
            // up in testing, which is exactly why it is not left to timing.
            const FStagingSlice& Slice = GUpload.Slices[Slot];
            for (;;)
            {
                {
                    FScopeLock Lock(GUpload.Mutex);
                    if (Slice.Writers.load(std::memory_order_acquire) == 0)
                    {
                        return;
                    }
                }
                Threading::ThreadYield();
            }
        }

        void NoteFlushSubmitted(uint32 SliceMask, FSemaphoreH Semaphore, uint64 Value)
        {
            for (uint32 Slot = 0; Slot < kFramesInFlight; ++Slot)
            {
                if ((SliceMask & (1u << Slot)) != 0)
                {
                    GUpload.Slices[Slot].ReadSemaphore = Semaphore;
                    GUpload.Slices[Slot].ReadValue     = Value;
                }
            }
        }
 
        void BeginSlot(uint32 Slot)
        {
            FStagingSlice& Slice = GUpload.Slices[Slot];

            // The copies out of this slice were submitted by the flush at the START of a later frame
            // than the one the frame ring waited on above, so that wait is one submission short of
            // covering them. Without this the cursor reset below hands live source bytes to the next
            // writer, and a mesh's vertex/index data ends up interleaved with the previous mesh's.
            if (Slice.ReadValue != 0)
            {
                WaitSemaphore(Slice.ReadSemaphore, Slice.ReadValue);
                Slice.ReadValue = 0;
            }

            FScopeLock Lock(GUpload.Mutex);
            GUpload.CurrentSlot = Slot;
            Slice.Cursor        = 0;
        }
    }
}
