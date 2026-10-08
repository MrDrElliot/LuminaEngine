#include "RuntimePCH.h"
#include "RHIUpload.h"
#include "RHICore.h"

#include "Core/Math/Math.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "Memory/Memcpy.h"
#include "Core/Profiler/Profile.h"

namespace Lumina::RHI
{
    namespace
    {
        // Twice what one frame used to stage, since a ring can hand the space back as soon as a batch finishes.
        constexpr uint64 kStagingRingRequest = 128 * Constants::kMiB;

        // A buffer upload past this goes in pieces, so one large write cannot occupy the whole ring.
        constexpr uint64 kRingPieceDivisor = 4;

        // Marks staging that lives outside the ring, which only a texture region larger than half the ring takes.
        constexpr uint64 kNotInRing = ~0ull;

        enum class EUploadOp : uint8 { Buffer, Texture, Clear, TextureCopy };

        struct FUploadOp
        {
            EUploadOp   Type;
            GPUPtr      Staging        = 0;         // source for Buffer/Texture, 0 for Clear/TextureCopy
            GPUPtr      BufferDest     = 0;         // Buffer
            FTextureH   TextureDest    = {};        // Texture/Clear/TextureCopy
            FTextureH   TextureSource  = {};        // TextureCopy
            uint64      Size           = 0;
            uint32      RowPitchTexels = 0;         // Texture
            uint32      Mip            = 0;         // the destination mip
            uint32      Layer          = 0;         // Texture/TextureCopy (array slice; 0 for non-array)
            uint32      SourceMip      = 0;         // TextureCopy
            uint32      SourceLayer    = 0;         // TextureCopy
            uint32      Width          = 0;         // mip extent, 0 derives from the description
            uint32      Height         = 0;
            uint32      OffsetY        = 0;         // first texel row this band writes
            float       ClearValue[4]  = {};        // Clear

            // Set only when the op reserved dedicated staging, which it frees once the copy retires.
            FGPUAllocation OwnedStaging = {};
        };

        static constexpr uint32 kNumUploadQueues = 3;

        // Positions are monotonic byte counts, so a position modulo the capacity is the offset in the buffer.
        struct FStagingRing
        {
            FGPUAllocation   Memory   = {};
            uint64           Capacity = 0;
            uint64           Head     = 0;
            uint64           Tail     = 0;

            // Reservations whose bytes are still being written, so no flush may count them as submitted.
            TVector<uint64>  ActiveStarts;

            uint64           PeakInUse   = 0;
            uint64           FullWaits   = 0;
            uint64           FullFlushes = 0;
            uint64           Oversized   = 0;
        };

        // One entry per swept flush, retired once every queue it was submitted on has signaled past it.
        struct FBatchGate
        {
            uint64      Batch   = 0;
            uint64      RingEnd = 0;   // every ring byte below this belongs to this batch or an earlier one
            FSemaphoreH Semaphore[kNumUploadQueues] = {};
            uint64      Value[kNumUploadQueues]     = {};
        };

        struct FUploadState
        {
            FStagingRing        Ring;
            TVector<FUploadOp>  Queue;
            // Parked between flushes so the queue is handed a buffer that already has capacity.
            TVector<FUploadOp>  QueueSpare;
            FMutex              Mutex;

            // Never held while taking the other locks, since the flush path already holds the core submit lock.
            FMutex              BatchMutex;
            uint64              BatchCounter     = 1;   // the flush queued ops will leave in
            uint64              CompletedBatch   = 0;   // every batch at or below this has executed
            uint64              CompletedRingEnd = 0;   // ring bytes below this are no longer read by any copy
            TVector<FBatchGate> InFlightBatches;

            TAtomic<uint32>     QueuedOps{0};

            bool                bInitialized = false;
        };

        FUploadState GUpload;

        struct FStaging
        {
            std::byte*  Cpu       = nullptr;
            GPUPtr      Gpu       = 0;
            uint64      RingStart = kNotInRing;

            // Non-null only for staging outside the ring, where the caller inherits the allocation.
            FGPUAllocation Owned = {};
        };

        // Caller holds BatchMutex. Batches execute in order per queue, so the first one still running blocks the rest.
        void PopCompletedBatchesLocked()
        {
            while (!GUpload.InFlightBatches.empty())
            {
                const FBatchGate& Gate = GUpload.InFlightBatches.front();

                bool bSubmitted = false;
                bool bExecuted  = true;
                for (uint32 QueueIndex = 0; QueueIndex < kNumUploadQueues; ++QueueIndex)
                {
                    if (Gate.Value[QueueIndex] == 0)
                    {
                        continue;
                    }

                    bSubmitted = true;
                    if (GetSemaphoreValue(Gate.Semaphore[QueueIndex]) < Gate.Value[QueueIndex])
                    {
                        bExecuted = false;
                        break;
                    }
                }

                // No recorded queue means the flush is unsubmitted, so the copies have not been issued.
                if (!bSubmitted || !bExecuted)
                {
                    break;
                }

                GUpload.CompletedBatch   = Gate.Batch;
                GUpload.CompletedRingEnd = Math::Max(GUpload.CompletedRingEnd, Gate.RingEnd);
                GUpload.InFlightBatches.erase(GUpload.InFlightBatches.begin());
            }
        }

        // Caller holds the upload mutex.
        uint64 SubmittableRingEndLocked()
        {
            const FStagingRing& Ring = GUpload.Ring;
            uint64 End = Ring.Head;
            for (uint64 Start : Ring.ActiveStarts)
            {
                End = Math::Min(End, Start);
            }
            return End;
        }

        struct FRingWait
        {
            FSemaphoreH Semaphore[kNumUploadQueues] = {};
            uint64      Value[kNumUploadQueues]     = {};
            bool        bAny = false;
        };

        enum class ERingStall : uint8 { Retry, Flush, Wait, Yield };

        // Caller holds the upload mutex. Frees what finished copies released, then says how to make room.
        ERingStall ReclaimRingLocked(FRingWait& OutWait)
        {
            FStagingRing& Ring = GUpload.Ring;
            const uint64 TailBefore = Ring.Tail;

            FScopeLock BatchLock(GUpload.BatchMutex);
            PopCompletedBatchesLocked();
            Ring.Tail = Math::Max(Ring.Tail, GUpload.CompletedRingEnd);

            // Nothing queued, writing or in flight means no copy can read the ring, so all of it is free.
            if (GUpload.InFlightBatches.empty() && GUpload.Queue.empty() && Ring.ActiveStarts.empty())
            {
                Ring.Tail = Ring.Head;
            }

            if (Ring.Tail != TailBefore)
            {
                return ERingStall::Retry;
            }

            for (const FBatchGate& Gate : GUpload.InFlightBatches)
            {
                if (Gate.RingEnd <= Ring.Tail)
                {
                    continue;
                }

                for (uint32 QueueIndex = 0; QueueIndex < kNumUploadQueues; ++QueueIndex)
                {
                    OutWait.Semaphore[QueueIndex] = Gate.Semaphore[QueueIndex];
                    OutWait.Value[QueueIndex]     = Gate.Value[QueueIndex];
                    OutWait.bAny |= Gate.Value[QueueIndex] != 0;
                }

                // Flushed but not yet submitted, which the flushing thread finishes in a moment.
                return OutWait.bAny ? ERingStall::Wait : ERingStall::Yield;
            }

            // The space is held by queued ops no flush has taken yet, or by writers still copying in.
            return GUpload.Queue.empty() ? ERingStall::Yield : ERingStall::Flush;
        }

        FStaging ReserveOutsideRing(uint64 Size, uint64 Alignment)
        {
            {
                FScopeLock Lock(GUpload.Mutex);
                if (GUpload.Ring.Oversized++ == 0)
                {
                    LOG_WARN("RHI: a {} MiB upload region is larger than half the {} MiB staging ring and takes its own "
                             "staging allocation.", Size >> 20, GUpload.Ring.Capacity >> 20);
                }
            }

            LUMINA_PROFILE_SECTION("Upload::OversizedStagingAlloc");
            const FGPUAllocation Owned = Malloc(Size, Alignment, EMemoryType::CPUWrite);
            SetDebugName(Owned.Gpu, "Upload.Oversized");
            return { Owned.Cpu, Owned.Gpu, kNotInRing, Owned };
        }

        // A full ring makes the caller wait for the oldest batch rather than allocate, so staging never grows.
        FStaging Reserve(uint64 Size, uint64 Alignment)
        {
            FStagingRing& Ring = GUpload.Ring;
            if (Size > Ring.Capacity / 2)
            {
                return ReserveOutsideRing(Size, Alignment);
            }

            for (;;)
            {
                FRingWait  Wait;
                ERingStall Stall = ERingStall::Retry;
                {
                    FScopeLock Lock(GUpload.Mutex);

                    const uint64 LapStart = Ring.Head - Ring.Head % Ring.Capacity;
                    const uint64 Offset   = Math::AlignUp(Ring.Head - LapStart, Alignment);

                    // A region never straddles the end of the buffer, so one that would starts the next lap.
                    const uint64 Start = Offset + Size <= Ring.Capacity ? LapStart + Offset : LapStart + Ring.Capacity;
                    if (Start + Size - Ring.Tail <= Ring.Capacity)
                    {
                        Ring.Head = Start + Size;
                        Ring.ActiveStarts.push_back(Start);
                        Ring.PeakInUse = Math::Max(Ring.PeakInUse, Ring.Head - Ring.Tail);

                        const uint64 RingOffset = Start % Ring.Capacity;
                        return { Ring.Memory.Cpu + RingOffset, Ring.Memory.Gpu + RingOffset, Start, {} };
                    }

                    Stall = ReclaimRingLocked(Wait);
                    Ring.FullFlushes += Stall == ERingStall::Flush ? 1 : 0;
                    Ring.FullWaits   += Stall == ERingStall::Wait ? 1 : 0;
                }

                switch (Stall)
                {
                case ERingStall::Retry:
                    break;
                case ERingStall::Flush:
                    FlushUploads();
                    break;
                case ERingStall::Wait:
                    {
                        // A wait here means uploads outran the GPU's copies, which is the backpressure doing its job.
                        LUMINA_PROFILE_SECTION("Upload::StagingRingFullWait");
                        for (uint32 QueueIndex = 0; QueueIndex < kNumUploadQueues; ++QueueIndex)
                        {
                            if (Wait.Value[QueueIndex] != 0)
                            {
                                WaitSemaphore(Wait.Semaphore[QueueIndex], Wait.Value[QueueIndex]);
                            }
                        }
                    }
                    break;
                case ERingStall::Yield:
                    Threading::ThreadYield();
                    break;
                }
            }
        }

        // Queues the op and ends the reservation together, so a flush never sees one without the other.
        void QueueStagedOp(const FUploadOp& Op, const FStaging& Staging)
        {
            FScopeLock Lock(GUpload.Mutex);
            GUpload.Queue.push_back(Op);
            GUpload.QueuedOps.store((uint32)GUpload.Queue.size(), std::memory_order_relaxed);

            if (Staging.RingStart != kNotInRing)
            {
                TVector<uint64>& Active = GUpload.Ring.ActiveStarts;
                for (SIZE_T i = 0; i < Active.size(); ++i)
                {
                    if (Active[i] == Staging.RingStart)
                    {
                        Active[i] = Active.back();
                        Active.pop_back();
                        break;
                    }
                }
            }
        }
    }

    bool UploadBuffer(const FGPUAllocation& Dest, const void* Data, uint64 Size, uint64 Offset)
    {
        if (Dest.Gpu == 0 || Data == nullptr || Size == 0)
        {
            return false;
        }

        ASSERT(Offset <= Dest.Size && Size <= Dest.Size - Offset, "buffer upload runs past its allocation");

        // A host-visible destination writes through the mapping with nothing to stage.
        if (Dest.Cpu != nullptr)
        {
            Memory::Memcpy(Dest.Cpu + Offset, Data, Size);
            return true;
        }

        const uint64 MaxPiece = Math::Max<uint64>(GUpload.Ring.Capacity / kRingPieceDivisor, kDefaultAlign);
        const uint8* Source   = static_cast<const uint8*>(Data);
        for (uint64 PieceOffset = 0; PieceOffset < Size; PieceOffset += MaxPiece)
        {
            const uint64 PieceSize = Math::Min(MaxPiece, Size - PieceOffset);
            const FStaging S = Reserve(PieceSize, kDefaultAlign);
            if (S.Cpu == nullptr)
            {
                LOG_ERROR("RHI: dropped a {} KiB buffer upload, staging allocation failed.", Size / 1024);
                return false;
            }

            Memory::MemcpyToWriteCombined(S.Cpu, Source + PieceOffset, PieceSize);

            FUploadOp Op;
            Op.Type          = EUploadOp::Buffer;
            Op.Staging       = S.Gpu;
            Op.OwnedStaging  = S.Owned;
            Op.BufferDest    = Dest.Gpu + Offset + PieceOffset;
            Op.Size          = PieceSize;
            QueueStagedOp(Op, S);
        }
        return true;
    }

    bool UploadTexture(FTextureH Dest, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels, uint32 Width, uint32 Height, uint32 OffsetY)
    {
        if (!IsValid(Dest) || Data == nullptr || Size == 0)
        {
            return false;
        }

        // A band with no explicit extent would read past the end of the source data.
        if (OffsetY != 0 && (Width == 0 || Height == 0))
        {
            LOG_ERROR("RHI: dropped a banded texture upload at row {} with no extent; Width/Height are "
                      "required whenever OffsetY is non-zero.", OffsetY);
            return false;
        }

        LUMINA_PROFILE_SECTION("Upload::StageTextureMip");
        LUMINA_PROFILE_VALUE("Upload/StagedMipKiB", (int64)(Size / 1024));

        // Split from the copy, since this arm is a cursor bump or a wait on a full ring.
        FStaging S;
        {
            LUMINA_PROFILE_SECTION("Upload::ReserveStaging");
            S = Reserve(Size, kDefaultAlign);
        }

        if (S.Cpu == nullptr)
        {
            LOG_ERROR("RHI: dropped a {} KiB texture upload, staging allocation failed.", Size / 1024);
            return false;
        }

        {
            // Isolated so slow staging can be told from slow staging allocation without guessing.
            LUMINA_PROFILE_SECTION("Upload::CopyToStaging");
            Memory::MemcpyToWriteCombined(S.Cpu, Data, Size);
        }

        FUploadOp Op;
        Op.Type           = EUploadOp::Texture;
        Op.Staging        = S.Gpu;
        Op.OwnedStaging   = S.Owned;
        Op.TextureDest    = Dest;
        Op.Size           = Size;
        Op.RowPitchTexels = RowPitchTexels;
        Op.Mip            = Mip;
        Op.Layer          = Layer;
        Op.Width          = Width;
        Op.Height         = Height;
        Op.OffsetY        = OffsetY;

        QueueStagedOp(Op, S);
        return true;
    }

    void UploadTextureCopy(FTextureH Dest, uint32 DestLayer, uint32 DestMip,
                           FTextureH Source, uint32 SourceLayer, uint32 SourceMip,
                           uint32 Width, uint32 Height)
    {
        if (!IsValid(Dest) || !IsValid(Source) || Width == 0 || Height == 0)
        {
            return;
        }

        // The path a mip takes when it already exists on the GPU and only moves between two images.
        FUploadOp Op;
        Op.Type          = EUploadOp::TextureCopy;
        Op.TextureDest   = Dest;
        Op.TextureSource = Source;
        Op.Mip           = DestMip;
        Op.Layer         = DestLayer;
        Op.SourceMip     = SourceMip;
        Op.SourceLayer   = SourceLayer;
        Op.Width         = Width;
        Op.Height        = Height;

        FScopeLock Lock(GUpload.Mutex);
        GUpload.Queue.push_back(Op);
        GUpload.QueuedOps.store((uint32)GUpload.Queue.size(), std::memory_order_relaxed);
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
        GUpload.QueuedOps.store((uint32)GUpload.Queue.size(), std::memory_order_relaxed);
    }

    void FlushUploadsAndWait()
    {
        if (!GUpload.bInitialized)
        {
            return;
        }

        TVector<FGPUAllocation> OwnedStaging;

        uint64 Batch = 0;
        const FCmdListH CL = OpenCommandList(EQueueType::Graphics);
        if (!Upload::Flush(CL, OwnedStaging, &Batch))
        {
            ResetCommandList(CL);
            return;
        }

        // Resetting here too would recycle one command buffer twice, which is a device loss.
        const uint64 Value = Submit(EQueueType::Graphics, TSpan<const FCmdListH>{&CL, 1});
        Upload::NoteFlushSubmitted(Batch, EQueueType::Graphics, GetQueueTimeline(EQueueType::Graphics), Value);

        WaitSemaphore(GetQueueTimeline(EQueueType::Graphics), Value);

        for (const FGPUAllocation& Staging : OwnedStaging)
        {
            Retire(Staging);
        }
    }

    void FlushUploads()
    {
        if (!GUpload.bInitialized || GUpload.QueuedOps.load(std::memory_order_relaxed) == 0)
        {
            return;
        }

        TVector<FGPUAllocation> OwnedStaging;

        uint64 Batch = 0;
        const FCmdListH CL = OpenCommandList(EQueueType::Graphics);
        if (!Upload::Flush(CL, OwnedStaging, &Batch))
        {
            ResetCommandList(CL);
            return;
        }

        const uint64 Value = Submit(EQueueType::Graphics, TSpan<const FCmdListH>{&CL, 1});
        Upload::NoteFlushSubmitted(Batch, EQueueType::Graphics, GetQueueTimeline(EQueueType::Graphics), Value);

        // After the submit, so each retire is gated on a queue value that covers the copy.
        for (const FGPUAllocation& Staging : OwnedStaging)
        {
            Retire(Staging);
        }
    }

    namespace Upload
    {
        void Initialize()
        {
            FStagingRing& Ring = GUpload.Ring;
            Ring.Capacity = ClampCPUWriteSlice("Staging", kStagingRingRequest, 1);
            Ring.Memory   = Malloc(Ring.Capacity, kDefaultAlign, EMemoryType::CPUWrite);
            SetDebugName(Ring.Memory.Gpu, "Upload.StagingRing");
            Ring.Head = 0;
            Ring.Tail = 0;
            Ring.ActiveStarts.clear();
            GUpload.bInitialized = true;
        }

        void Shutdown()
        {
            if (!GUpload.bInitialized)
            {
                return;
            }

            WaitDeviceIdle();

            // Anything still queued never reached the GPU; retire any dedicated staging it owns.
            TVector<FGPUAllocation> Orphaned;
            {
                FScopeLock Lock(GUpload.Mutex);
                for (const FUploadOp& Op : GUpload.Queue)
                {
                    if (Op.OwnedStaging.Gpu != 0)
                    {
                        Orphaned.push_back(Op.OwnedStaging);
                    }
                }
                GUpload.Queue.clear();
            }

            // Outside the lock, since Retire re-enters CancelBuffer, which takes the upload mutex.
            for (const FGPUAllocation& Staging : Orphaned)
            {
                Retire(Staging);
            }

            Retire(GUpload.Ring.Memory);
            GUpload.Ring = FStagingRing{};

            // Device idle above, so every recorded gate has executed by definition.
            {
                FScopeLock Lock(GUpload.BatchMutex);
                GUpload.CompletedBatch   = GUpload.BatchCounter;
                GUpload.CompletedRingEnd = 0;
                GUpload.InFlightBatches.clear();
            }

            GUpload.bInitialized = false;
        }

        // Bounds the linear written-list scans; retiring a window early with a barrier is always safe.
        constexpr SIZE_T kMaxWrittenTracked = 128;

        struct FFlushTarget
        {
            struct FWrittenRange { GPUPtr Begin; GPUPtr End; };

            FCmdListH CL   = {};
            bool      bAny = false;

            // Inline, since the window above bounds both lists and a flush runs every frame.
            TFixedVector<FTextureH, kMaxWrittenTracked>    WrittenTextures;
            TFixedVector<FWrittenRange, kMaxWrittenTracked> WrittenBuffers;

            bool AlreadyWritten(FTextureH Tex) const
            {
                for (const FTextureH& T : WrittenTextures)
                {
                    if (T.Handle == Tex.Handle)
                    {
                        return true;
                    }
                }
                return false;
            }

            bool OverlapsWritten(GPUPtr Dest, uint64 Size) const
            {
                for (const FWrittenRange& Range : WrittenBuffers)
                {
                    if (Dest < Range.End && Range.Begin < Dest + Size)
                    {
                        return true;
                    }
                }
                return false;
            }
        };

        uint32 FlushSplit(FCmdListH BufferCL, FCmdListH ImageCL, TVector<FGPUAllocation>& OutOwnedStaging, uint64* OutBatch)
        {
            LUMINA_PROFILE_SECTION("Upload::FlushSplit");

            TVector<FUploadOp> Ops;
            uint64 Batch = 0;
            {
                FScopeLock Lock(GUpload.Mutex);
                if (GUpload.Queue.empty())
                {
                    return 0u;
                }
                Ops.swap(GUpload.Queue);

                // Without this the queue restarts from zero capacity and regrows through every flush.
                GUpload.Queue.swap(GUpload.QueueSpare);
                GUpload.QueuedOps.store(0u, std::memory_order_relaxed);

                // Under the queue lock, so BatchForQueuedOps cannot hand out a batch whose ops already went out.
                FScopeLock BatchLock(GUpload.BatchMutex);
                Batch = GUpload.BatchCounter++;
                GUpload.InFlightBatches.push_back(FBatchGate{ .Batch = Batch, .RingEnd = SubmittableRingEndLocked() });
            }

            if (OutBatch != nullptr)
            {
                *OutBatch = Batch;
            }

            const bool bSplit = (BufferCL.Handle != ImageCL.Handle);

            FFlushTarget Targets[2];
            Targets[0].CL = BufferCL;
            Targets[1].CL = ImageCL;

            for (const FUploadOp& Op : Ops)
            {
                const bool bWritesTexture = (Op.Type != EUploadOp::Buffer);
                const bool bWritesBuffer  = (Op.Type == EUploadOp::Buffer);

                FFlushTarget& T = (bSplit && bWritesBuffer) ? Targets[0] : Targets[1];

                // In-place updates overwrite what the previous frame may still be reading on this queue.
                if (!T.bAny)
                {
                    RHI::CmdBarrier(T.CL,
                        RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader |
                        RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::FragmentTests | RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::Transfer,
                        RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite | RHI::EAccessFlags::DepthStencilWrite | RHI::EAccessFlags::TransferWrite,
                        RHI::EStageFlags::Transfer,
                        RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
                }
                T.bAny = true;

                // A copy reads as well as writes, so an earlier write to its source has to be ordered against.
                const bool bReadsWritten = Op.Type == EUploadOp::TextureCopy && T.AlreadyWritten(Op.TextureSource);

                const bool bWindowFull = T.WrittenTextures.size() >= kMaxWrittenTracked
                                      || T.WrittenBuffers.size() >= kMaxWrittenTracked;

                if (bWindowFull
                 || bReadsWritten
                 || (bWritesTexture && T.AlreadyWritten(Op.TextureDest))
                 || (bWritesBuffer && T.OverlapsWritten(Op.BufferDest, Op.Size)))
                {
                    Barriers::TransferToTransfer(T.CL);
                    T.WrittenTextures.clear();
                    T.WrittenBuffers.clear();
                }

                switch (Op.Type)
                {
                case EUploadOp::Buffer:
                    CmdMemcpy(T.CL, { Op.BufferDest, Op.Size }, { Op.Staging, Op.Size });
                    break;
                case EUploadOp::Texture:
                    {
                        FTextureSlice Slice;
                        Slice.Mip        = Op.Mip;
                        Slice.Layer      = Op.Layer;
                        Slice.LayerCount = 1;
                        Slice.Offset     = FUIntVector3(0u, Op.OffsetY, 0u);
                        if (Op.Width != 0)
                        {
                            Slice.Extent = FUIntVector3(Op.Width, Math::Max(Op.Height, 1u), 1u);
                        }
                        CmdCopyMemoryToTexture(T.CL, { Op.Staging, Op.Size }, Op.RowPitchTexels, Op.TextureDest, Slice);
                    }
                    break;
                case EUploadOp::Clear:
                    CmdClearTexture(T.CL, Op.TextureDest, Op.ClearValue);
                    break;
                case EUploadOp::TextureCopy:
                    {
                        FTextureSlice Src;
                        Src.Mip        = Op.SourceMip;
                        Src.Layer      = Op.SourceLayer;
                        Src.LayerCount = 1;
                        Src.Extent     = FUIntVector3(Op.Width, Op.Height, 1u);

                        FTextureSlice Dst = Src;
                        Dst.Mip   = Op.Mip;
                        Dst.Layer = Op.Layer;

                        CmdCopyTexture(T.CL, Op.TextureSource, Src, Op.TextureDest, Dst);
                    }
                    break;
                }

                if (bWritesTexture)
                {
                    T.WrittenTextures.push_back(Op.TextureDest);
                }
                if (bWritesBuffer)
                {
                    T.WrittenBuffers.push_back(FFlushTarget::FWrittenRange{ Op.BufferDest, Op.BufferDest + Op.Size });
                }
            }

            uint32 Result = 0u;
            for (uint32 i = 0; i < 2u; ++i)
            {
                if (!Targets[i].bAny)
                {
                    continue;
                }
                RHI::CmdBarrier(Targets[i].CL,
                    RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                    RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::Transfer |
                    RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::FragmentTests,
                    RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite | RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead | RHI::EAccessFlags::IndexRead |
                    RHI::EAccessFlags::ColorRead | RHI::EAccessFlags::ColorWrite | RHI::EAccessFlags::DepthStencilRead | RHI::EAccessFlags::DepthStencilWrite);
                Result |= (1u << i);
            }
            
            for (const FUploadOp& Op : Ops)
            {
                if (Op.OwnedStaging.Gpu != 0)
                {
                    OutOwnedStaging.push_back(Op.OwnedStaging);
                }
            }

            // Hands the drained buffer back so the next flush swaps it in rather than allocating.
            {
                FScopeLock Lock(GUpload.Mutex);
                Ops.clear();
                GUpload.QueueSpare.swap(Ops);
            }

            return Result;
        }

        bool Flush(FCmdListH CL, TVector<FGPUAllocation>& OutOwnedStaging, uint64* OutBatch)
        {
            return FlushSplit(CL, CL, OutOwnedStaging, OutBatch) != 0u;
        }

        template<typename TPredicate>
        static void CancelMatching(TPredicate&& Targets)
        {
            TVector<FGPUAllocation> Orphaned;
            {
                FScopeLock Lock(GUpload.Mutex);

                size_t Write = 0;
                for (size_t Read = 0; Read < GUpload.Queue.size(); ++Read)
                {
                    FUploadOp& Op = GUpload.Queue[Read];
                    if (Targets(Op))
                    {
                        if (Op.OwnedStaging.Gpu != 0)
                        {
                            Orphaned.push_back(Op.OwnedStaging);
                        }
                        continue;
                    }

                    if (Write != Read)
                    {
                        GUpload.Queue[Write] = GUpload.Queue[Read];   // trivially copyable; no owned members
                    }
                    ++Write;
                }

                GUpload.Queue.resize(Write);
                GUpload.QueuedOps.store((uint32)Write, std::memory_order_relaxed);
            }
            
            for (const FGPUAllocation& Staging : Orphaned)
            {
                Retire(Staging);
            }
        }

        void CancelTexture(FTextureH Texture)
        {
            if (!GUpload.bInitialized || !IsValid(Texture) || GUpload.QueuedOps.load(std::memory_order_relaxed) == 0u)
            {
                return;
            }

            // A queued copy reading a dying image records against a dead handle exactly as a write would.
            CancelMatching([Texture](const FUploadOp& Op)
            {
                return Op.Type != EUploadOp::Buffer
                    && (Op.TextureDest.Handle == Texture.Handle || Op.TextureSource.Handle == Texture.Handle);
            });
        }

        void CancelBuffer(const FGPUAllocation& Dest)
        {
            if (!GUpload.bInitialized || Dest.Gpu == 0 || GUpload.QueuedOps.load(std::memory_order_relaxed) == 0u)
            {
                return;
            }

            const GPUPtr Base = Dest.Gpu;
            const GPUPtr End  = Base + Dest.Size;
            
            CancelMatching([Base, End](const FUploadOp& Op)
            {
                return Op.Type == EUploadOp::Buffer && Op.BufferDest < End && Base < Op.BufferDest + Op.Size;
            });
        }

        void NoteFlushSubmitted(uint64 Batch, EQueueType Queue, FSemaphoreH Semaphore, uint64 Value)
        {
            const uint32 QueueIndex = (uint32)Queue;

            // A flush can straddle two queues, so the gate accumulates rather than overwrites.
            FScopeLock Lock(GUpload.BatchMutex);
            for (FBatchGate& Gate : GUpload.InFlightBatches)
            {
                if (Gate.Batch == Batch)
                {
                    Gate.Semaphore[QueueIndex] = Semaphore;
                    Gate.Value[QueueIndex]     = Value;
                    return;
                }
            }
        }

        uint64 BatchForQueuedOps()
        {
            FScopeLock Lock(GUpload.Mutex);
            FScopeLock BatchLock(GUpload.BatchMutex);

            // Returning the open batch would wait on a flush that may never happen, since no ops means no flush.
            if (GUpload.Queue.empty())
            {
                return GUpload.BatchCounter - 1;
            }
            return GUpload.BatchCounter;
        }

        bool IsBatchComplete(uint64 Batch)
        {
            if (Batch == 0 || !GUpload.bInitialized)
            {
                return true;
            }

            FScopeLock Lock(GUpload.BatchMutex);
            if (Batch <= GUpload.CompletedBatch)
            {
                return true;
            }

            PopCompletedBatchesLocked();
            return Batch <= GUpload.CompletedBatch;
        }

        void LogStats(FStringView Label)
        {
            FScopeLock Lock(GUpload.Mutex);
            const FStagingRing& Ring = GUpload.Ring;
            LOG_DISPLAY("RHI upload ring [{}]: {} MiB, {} MiB peak in use, {} waits on the GPU, {} flushes to make room, "
                        "{} regions staged outside the ring.",
                Label, Ring.Capacity >> 20, Ring.PeakInUse >> 20, Ring.FullWaits, Ring.FullFlushes, Ring.Oversized);
        }

        void PublishProfileCounters()
        {
            FScopeLock Lock(GUpload.Mutex);
            LUMINA_PROFILE_VALUE("Upload/StagingInUseKiB", (int64)((GUpload.Ring.Head - GUpload.Ring.Tail) / 1024));
        }
    }
}
