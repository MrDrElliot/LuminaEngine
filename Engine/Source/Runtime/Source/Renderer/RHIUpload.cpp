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
        // Typed rather than a suffix literal, see the note in RHICore.cpp.
        constexpr uint64 kMegabyte = 1024 * 1024;

        constexpr uint64 kStagingSliceRequest = 64 * kMegabyte;

        // Resolved against the CPU-visible VRAM aperture in Initialize.
        uint64 GStagingSliceSize = kStagingSliceRequest;

        // Staging that came from a dedicated allocation rather than one of the slices.
        constexpr uint8 kNoSlice = 0xFF;

        enum class EUploadOp : uint8 { Buffer, Texture, Clear, TextureCopy };

        struct FUploadOp
        {
            EUploadOp   Type;
            GPUPtr      Staging        = 0;         // source for Buffer/Texture, 0 for Clear/TextureCopy
            uint8       Slice          = kNoSlice;  // slice the staging was reserved from
            GPUPtr      BufferDest     = 0;         // Buffer
            FTextureH   TextureDest    = {};        // Texture/Clear/TextureCopy
            FTextureH   TextureSource  = {};        // TextureCopy
            uint64      Size           = 0;
            uint32      RowPitchTexels = 0;         // Texture
            uint32      Mip            = 0;         // the DESTINATION mip
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

        struct FStagingSlice
        {
            FGPUAllocation Memory = {};
            uint64      Cursor   = 0;
            uint64      Capacity = 0;   // resolved slice size; 0 until Initialize runs

            TAtomic<uint32> Writers{0};

            // One gate per queue, since a flush splits across two timelines whose values are unrelated.
            FSemaphoreH ReadSemaphore[kNumUploadQueues] = {};
            uint64      ReadValue[kNumUploadQueues]     = {};

            uint64      OverflowBytes = 0;
            uint32      LowStreak     = 0;   // consecutive cycles demand stayed below half capacity

            // Latched so a slice that cannot grow says so once instead of once per frame forever.
            bool        bWarnedGrowFailed = false;
        };

        // One entry per swept flush, retired once every queue it was submitted on has signaled past it.
        struct FBatchGate
        {
            uint64      Batch = 0;
            FSemaphoreH Semaphore[kNumUploadQueues] = {};
            uint64      Value[kNumUploadQueues]     = {};
        };

        struct FUploadState
        {
            FStagingSlice       Slices[kFramesInFlight];
            uint32              CurrentSlot = 0;
            TVector<FUploadOp>  Queue;
            // Parked between flushes so the queue is handed a buffer that already has capacity.
            TVector<FUploadOp>  QueueSpare;
            FMutex              Mutex;

            // Never held while taking the other locks, since the flush path already holds the core submit lock.
            FMutex              BatchMutex;
            uint64              BatchCounter   = 1;   // the flush queued ops will leave in
            uint64              CompletedBatch = 0;   // every batch at or below this has executed
            TVector<FBatchGate> InFlightBatches;

            TAtomic<uint32>     QueuedOps{0};

            bool                bInitialized = false;
        };

        FUploadState GUpload;

        struct FStaging
        {
            std::byte*  Cpu    = nullptr;
            GPUPtr      Gpu    = 0;
            uint8       Slice  = kNoSlice;

            // Non-null only on the overflow path, where the caller inherits the allocation.
            FGPUAllocation Owned = {};
        };

        FStaging ReserveLocked(uint64 Size, uint64 Alignment)
        {
            const uint32 Slot = GUpload.CurrentSlot;
            FStagingSlice& Slice = GUpload.Slices[Slot];
            const uint64 Aligned = Math::AlignUp(Slice.Cursor, Alignment);
            if (Aligned + Size <= Slice.Capacity)
            {
                Slice.Cursor = Aligned + Size;
                Slice.Writers.fetch_add(1, std::memory_order_relaxed);
                return { Slice.Memory.Cpu + Aligned, Slice.Memory.Gpu + Aligned, (uint8)Slot, {} };
            }

            Slice.OverflowBytes += Size + Alignment;

            // A separate zone, since a fresh host-visible allocation is a different cost from a ring write.
            LUMINA_PROFILE_SECTION("Upload::StagingOverflowAlloc");
            const FGPUAllocation Owned = Malloc(Size, Alignment, EMemoryType::CPUWrite);
            SetDebugName(Owned.Gpu, "Upload.Overflow");
            return { Owned.Cpu, Owned.Gpu, kNoSlice, Owned };
        }

        void EndWrite(const FStaging& Staging)
        {
            if (Staging.Slice != kNoSlice)
            {
                GUpload.Slices[Staging.Slice].Writers.fetch_sub(1, std::memory_order_release);
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

        FStaging S;
        {
            FScopeLock Lock(GUpload.Mutex);
            S = ReserveLocked(Size, kDefaultAlign);
        }

        if (S.Cpu == nullptr)
        {
            LOG_ERROR("RHI: dropped a {} KiB buffer upload, staging allocation failed.", Size / 1024);
            return false;
        }

        Memory::MemcpyToWriteCombined(S.Cpu, Data, Size);

        FUploadOp Op;
        Op.Type          = EUploadOp::Buffer;
        Op.Staging       = S.Gpu;
        Op.OwnedStaging  = S.Owned;
        Op.Slice         = S.Slice;
        Op.BufferDest    = Dest.Gpu + Offset;
        Op.Size          = Size;

        {
            FScopeLock Lock(GUpload.Mutex);
            GUpload.Queue.push_back(Op);
            GUpload.QueuedOps.store((uint32)GUpload.Queue.size(), std::memory_order_relaxed);
        }

        EndWrite(S);
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

        // Split from the copy, since this arm is a cursor bump or a fresh allocation on overflow.
        FStaging S;
        {
            LUMINA_PROFILE_SECTION("Upload::ReserveStaging");
            FScopeLock Lock(GUpload.Mutex);
            S = ReserveLocked(Size, kDefaultAlign);
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
        Op.Slice          = S.Slice;
        Op.TextureDest    = Dest;
        Op.Size           = Size;
        Op.RowPitchTexels = RowPitchTexels;
        Op.Mip            = Mip;
        Op.Layer          = Layer;
        Op.Width          = Width;
        Op.Height         = Height;
        Op.OffsetY        = OffsetY;

        {
            FScopeLock Lock(GUpload.Mutex);
            GUpload.Queue.push_back(Op);
            GUpload.QueuedOps.store((uint32)GUpload.Queue.size(), std::memory_order_relaxed);
        }

        EndWrite(S);
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
        if (!Upload::Flush(CL, OwnedStaging, nullptr, &Batch))
        {
            ResetCommandList(CL);
            return;
        }

        // Resetting here too would recycle one command buffer twice, which is a device loss.
        const uint64 Value = Core::SubmitOn(EQueueType::Graphics, TSpan<const FCmdListH>{&CL, 1});
        Upload::NoteFlushSubmitted(Batch, 0, EQueueType::Graphics, Core::GetQueueTimeline(EQueueType::Graphics), Value);

        WaitSemaphore(Core::GetQueueTimeline(EQueueType::Graphics), Value);

        for (const FGPUAllocation& Staging : OwnedStaging)
        {
            Core::Retire(Staging);
        }
    }

    namespace Upload
    {
        void Initialize()
        {
            GStagingSliceSize = ClampCPUWriteSlice("Staging", kStagingSliceRequest, kFramesInFlight);

            for (FStagingSlice& Slice : GUpload.Slices)
            {
                Slice.Memory   = Malloc(GStagingSliceSize, kDefaultAlign, EMemoryType::CPUWrite);
                SetDebugName(Slice.Memory.Gpu, "Upload.StagingSlice");
                Slice.Cursor   = 0;
                Slice.Capacity = GStagingSliceSize;
                Slice.Writers.store(0, std::memory_order_relaxed);
                for (uint64& Value : Slice.ReadValue)
                {
                    Value = 0;
                }
            }
            GUpload.CurrentSlot  = 0;
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

            // Outside the lock, since Core::Retire re-enters CancelBuffer, which takes the upload mutex.
            for (const FGPUAllocation& Staging : Orphaned)
            {
                Core::Retire(Staging);
            }

            for (FStagingSlice& Slice : GUpload.Slices)
            {
                Core::Retire(Slice.Memory);
                Slice.Memory   = {};
                Slice.Cursor   = 0;
                Slice.Capacity = 0;
                Slice.Writers.store(0, std::memory_order_relaxed);
                for (FSemaphoreH& Semaphore : Slice.ReadSemaphore)
                {
                    Semaphore = {};
                }
                for (uint64& Value : Slice.ReadValue)
                {
                    Value = 0;
                }
            }

            // Device idle above, so every recorded gate has executed by definition.
            {
                FScopeLock Lock(GUpload.BatchMutex);
                GUpload.CompletedBatch = GUpload.BatchCounter;
                GUpload.InFlightBatches.clear();
            }

            GUpload.bInitialized = false;
        }

        // Bounds the linear written-list scans; retiring a window early with a barrier is always safe.
        constexpr SIZE_T kMaxWrittenTracked = 128;

        struct FFlushTarget
        {
            struct FWrittenRange { GPUPtr Begin; GPUPtr End; };

            FCmdListH CL        = {};
            uint32    SliceMask = 0;
            bool      bAny      = false;

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

        uint32 FlushSplit(FCmdListH BufferCL, FCmdListH ImageCL,
                          uint32* OutBufferSliceMask, uint32* OutImageSliceMask,
                          TVector<FGPUAllocation>& OutOwnedStaging, uint64* OutBatch)
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
                GUpload.InFlightBatches.push_back(FBatchGate{ Batch });
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
                T.bAny = true;

                if (Op.Slice != kNoSlice)
                {
                    T.SliceMask |= (1u << Op.Slice);
                }

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
                    CmdMemcpy(T.CL, Op.BufferDest, Op.Staging, Op.Size);
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
                        CmdCopyMemoryToTexture(T.CL, Op.Staging, Op.RowPitchTexels, Op.TextureDest, Slice);
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
                Barriers::TransferToAll(Targets[i].CL);
                Result |= (1u << i);
            }
            
            for (const FUploadOp& Op : Ops)
            {
                if (Op.OwnedStaging.Gpu != 0)
                {
                    OutOwnedStaging.push_back(Op.OwnedStaging);
                }
            }

            if (OutBufferSliceMask != nullptr)
            {
                *OutBufferSliceMask = Targets[0].SliceMask;
            }
            if (OutImageSliceMask != nullptr)
            {
                *OutImageSliceMask = Targets[1].SliceMask;
            }

            // Hands the drained buffer back so the next flush swaps it in rather than allocating.
            {
                FScopeLock Lock(GUpload.Mutex);
                Ops.clear();
                GUpload.QueueSpare.swap(Ops);
            }

            return Result;
        }

        bool Flush(FCmdListH CL, TVector<FGPUAllocation>& OutOwnedStaging, uint32* OutSliceMask, uint64* OutBatch)
        {
            uint32 BufferSlices = 0;
            uint32 ImageSlices  = 0;
            const uint32 Used = FlushSplit(CL, CL, &BufferSlices, &ImageSlices, OutOwnedStaging, OutBatch);

            if (OutSliceMask != nullptr)
            {
                *OutSliceMask = BufferSlices | ImageSlices;
            }
            return Used != 0u;
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
                Core::Retire(Staging);
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

        void DrainSliceWriters(uint32 Slot)
        {
            if (!GUpload.bInitialized)
            {
                return;
            }

            // Blocks the frame on worker threads, not on the GPU, and it sits inside Core::BeginFrame.
            LUMINA_PROFILE_SECTION_COLORED("Upload::DrainSliceWriters (Workers)", tracy::Color::Goldenrod);

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

        void NoteFlushSubmitted(uint64 Batch, uint32 SliceMask, EQueueType Queue, FSemaphoreH Semaphore, uint64 Value)
        {
            const uint32 QueueIndex = (uint32)Queue;
            for (uint32 Slot = 0; Slot < kFramesInFlight; ++Slot)
            {
                if ((SliceMask & (1u << Slot)) != 0)
                {
                    GUpload.Slices[Slot].ReadSemaphore[QueueIndex] = Semaphore;
                    GUpload.Slices[Slot].ReadValue[QueueIndex]     = Value;
                }
            }

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

            // Batches submit in order per queue, so the first still running blocks the rest.
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

                GUpload.CompletedBatch = Gate.Batch;
                GUpload.InFlightBatches.erase(GUpload.InFlightBatches.begin());
            }

            return Batch <= GUpload.CompletedBatch;
        }

        void BeginSlot(uint32 Slot)
        {
            FStagingSlice& Slice = GUpload.Slices[Slot];

            for (uint32 QueueIndex = 0; QueueIndex < kNumUploadQueues; ++QueueIndex)
            {
                const uint64 Gate = Slice.ReadValue[QueueIndex];
                if (Gate == 0)
                {
                    continue;
                }

                if (GetSemaphoreValue(Slice.ReadSemaphore[QueueIndex]) < Gate)
                {
                    // A hitch here means the frame caught up with the upload, not that the upload was slow.
                    LUMINA_PROFILE_SECTION("Upload::BeginSlot Wait");
                    WaitSemaphore(Slice.ReadSemaphore[QueueIndex], Gate);
                }
                Slice.ReadValue[QueueIndex] = 0;
            }

            FGPUAllocation OldMemory = {};
            {
                FScopeLock Lock(GUpload.Mutex);

                const uint64 Demand = Slice.Cursor + Slice.OverflowBytes;

                uint64 NewCapacity = Slice.Capacity;
                if (Demand > Slice.Capacity)
                {
                    NewCapacity     = Math::AlignUp(Demand + Demand / 2, 1024ull * 1024);
                    Slice.LowStreak = 0;
                }
                else if (Slice.Capacity > GStagingSliceSize && Demand * 2 < Slice.Capacity)
                {
                    if (++Slice.LowStreak >= 64)
                    {
                        NewCapacity     = Math::Max(GStagingSliceSize, Math::AlignUp(Demand + Demand / 2, kMegabyte));
                        Slice.LowStreak = 0;
                    }
                }
                else
                {
                    Slice.LowStreak = 0;
                }

                if (NewCapacity != Slice.Capacity)
                {
                    const FGPUAllocation NewMemory = Malloc(NewCapacity, kDefaultAlign, EMemoryType::CPUWrite);
                    if (NewMemory.Gpu != 0)
                    {
                        SetDebugName(NewMemory.Gpu, "Upload.StagingSlice");
                        OldMemory      = Slice.Memory;
                        Slice.Memory   = NewMemory;
                        Slice.Capacity = NewCapacity;
                        Slice.bWarnedGrowFailed = false;
                    }
                    else if (NewCapacity > Slice.Capacity && !Slice.bWarnedGrowFailed)
                    {
                        // Silent before this, so every oversized upload paid a fresh allocation on the calling thread.
                        Slice.bWarnedGrowFailed = true;
                        LOG_WARN("RHI: upload staging slice could not grow {} -> {} MiB (CPU-visible VRAM "
                                 "exhausted). Uploads larger than the slice will allocate dedicated staging "
                                 "on the calling thread until demand drops.",
                            Slice.Capacity >> 20, NewCapacity >> 20);
                    }
                }

                // Non-zero means uploads are falling out of the ring and paying for their own allocation.
                LUMINA_PROFILE_VALUE("Upload/StagingOverflowKiB", (int64)(Slice.OverflowBytes / 1024));
                LUMINA_PROFILE_VALUE("Upload/StagingUsedKiB",     (int64)(Slice.Cursor / 1024));

                GUpload.CurrentSlot   = Slot;
                Slice.Cursor          = 0;
                Slice.OverflowBytes   = 0;
            }

            // Outside the lock, since Core::Retire re-enters CancelBuffer, which takes the upload mutex.
            Core::Retire(OldMemory);
        }
    }
}
