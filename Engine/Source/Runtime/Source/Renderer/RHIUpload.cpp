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
        constexpr uint64 kStagingSliceRequest = 64ull * 1024 * 1024;

        // Resolved against the CPU-visible VRAM aperture in Initialize.
        uint64 GStagingSliceSize = kStagingSliceRequest;

        // Staging that came from a dedicated allocation rather than one of the slices.
        constexpr uint8 kNoSlice = 0xFF;

        enum class EUploadOp : uint8 { Buffer, Texture, Clear, TextureCopy };

        struct FUploadOp
        {
            EUploadOp   Type;
            GPUPtr      Staging        = 0;         // source for Buffer/Texture, 0 for Clear/TextureCopy
            bool        bOwnedStaging  = false;     // true -> DeferredFree after the copy retires
            uint8       Slice          = kNoSlice;  // slice the staging was reserved from
            GPUPtr      BufferDest     = 0;         // Buffer
            FTextureH   TextureDest    = {};        // Texture/Clear/TextureCopy
            FTextureH   TextureSource  = {};        // TextureCopy
            uint64      Size           = 0;
            uint32      RowPitchTexels = 0;         // Texture
            uint32      Mip            = 0;         // Texture/TextureCopy: DESTINATION mip
            uint32      Layer          = 0;         // Texture/TextureCopy (array slice; 0 for non-array)
            uint32      SourceMip      = 0;         // TextureCopy
            uint32      SourceLayer    = 0;         // TextureCopy
            uint32      Width          = 0;         // Texture: mip extent, 0 = derive from the description
            uint32      Height         = 0;
            uint32      OffsetY        = 0;         // Texture: first texel row this band writes
            float       ClearValue[4]  = {};        // Clear
        };

        static constexpr uint32 kNumUploadQueues = 3;

        struct FStagingSlice
        {
            GPUPtr      Gpu      = 0;
            std::byte*  Cpu      = nullptr;
            uint64      Cursor   = 0;
            uint64      Capacity = 0;   // resolved slice size; 0 until Initialize runs

            TAtomic<uint32> Writers{0};

            // One gate per queue: a flush splits across two timelines whose values are unrelated.
            FSemaphoreH ReadSemaphore[kNumUploadQueues] = {};
            uint64      ReadValue[kNumUploadQueues]     = {};

            uint64      OverflowBytes = 0;
            uint32      LowStreak     = 0;   // consecutive cycles demand stayed below half capacity

            // Latched so a slice that cannot grow says so once instead of once per frame forever.
            bool        bWarnedGrowFailed = false;
        };

        // A flush's completion gate. One entry per flush that has been swept out of the queue, retired once
        // every queue it was submitted on has signalled past it.
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
            FMutex              Mutex;

            // Guards the batch ledger only, and is never held while taking Mutex or any RHI lock -- the
            // flush submit path already holds the core submit lock when it records a gate.
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
            bool        bOwned = false;
            uint8       Slice  = kNoSlice;
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
                return { Slice.Cpu + Aligned, Slice.Gpu + Aligned, false, (uint8)Slot };
            }

            Slice.OverflowBytes += Size + Alignment;

            // Separate zone: this arm is a fresh host-visible VRAM allocation on the CALLING thread,
            // which is a completely different cost from writing into the ring.
            LUMINA_PROFILE_SECTION("Upload::StagingOverflowAlloc");
            const GPUPtr Owned = Malloc(Size, Alignment, EMemoryType::CPUWrite);
            SetDebugName(Owned, "Upload.Overflow");
            return { static_cast<std::byte*>(ToHost(Owned)), Owned, true, kNoSlice };
        }

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

        Memory::MemcpyToWriteCombined(S.Cpu, Data, Size);

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
            GUpload.QueuedOps.store((uint32)GUpload.Queue.size(), std::memory_order_relaxed);
        }

        EndWrite(S);
    }

    void UploadTexture(FTextureH Dest, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels, uint32 Width, uint32 Height, uint32 OffsetY)
    {
        if (!IsValid(Dest) || Data == nullptr || Size == 0)
        {
            return;
        }

        // A band with no explicit extent would be recorded against the whole mip and read Size bytes past
        // the end of Data. Refuse rather than hand the copy engine an over-long region.
        if (OffsetY != 0 && (Width == 0 || Height == 0))
        {
            LOG_ERROR("RHI: dropped a banded texture upload at row {} with no extent; Width/Height are "
                      "required whenever OffsetY is non-zero.", OffsetY);
            return;
        }

        LUMINA_PROFILE_SECTION("Upload::StageTextureMip");
        LUMINA_PROFILE_VALUE("Upload/StagedMipKiB", (int64)(Size / 1024));

        // Split from the copy below because they fail differently and cost differently: this arm is a lock
        // plus a cursor bump in the common case and a fresh host-visible VRAM allocation in the overflow
        // case, which is the one that turns a staging call into a multi-millisecond stall.
        FStaging S;
        {
            LUMINA_PROFILE_SECTION("Upload::ReserveStaging");
            FScopeLock Lock(GUpload.Mutex);
            S = ReserveLocked(Size, kDefaultAlign);
        }

        if (S.Cpu == nullptr)
        {
            LOG_ERROR("RHI: dropped a {} KiB texture upload, staging allocation failed.", Size / 1024);
            return;
        }

        {
            // The host bandwidth itself, isolated so "staging is slow" can be told apart from "allocating
            // staging is slow" without guessing. This is the cost MaxUploadMBPerFrame is meant to bound.
            LUMINA_PROFILE_SECTION("Upload::CopyToStaging");
            Memory::MemcpyToWriteCombined(S.Cpu, Data, Size);
        }

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
        Op.Width          = Width;
        Op.Height         = Height;
        Op.OffsetY        = OffsetY;

        {
            FScopeLock Lock(GUpload.Mutex);
            GUpload.Queue.push_back(Op);
            GUpload.QueuedOps.store((uint32)GUpload.Queue.size(), std::memory_order_relaxed);
        }

        EndWrite(S);
    }

    void UploadTextureCopy(FTextureH Dest, uint32 DestLayer, uint32 DestMip,
                           FTextureH Source, uint32 SourceLayer, uint32 SourceMip,
                           uint32 Width, uint32 Height)
    {
        if (!IsValid(Dest) || !IsValid(Source) || Width == 0 || Height == 0)
        {
            return;
        }

        // No staging and no host bandwidth at all: this is the path a mip takes when it already exists on
        // the GPU and is only moving between two images, which is most of what a residency change does.
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

        TVector<GPUPtr> OwnedStaging;

        uint64 Batch = 0;
        const FCmdListH CL = OpenCommandList(EQueueType::Graphics);
        if (!Upload::Flush(CL, OwnedStaging, nullptr, &Batch))
        {
            ResetCommandList(CL);
            return;
        }

        // Through Core, not straight to the backend: a submission that does not advance the queue counter
        // is invisible to the retire fence, which would then free resources this command list still reads.
        //
        // SubmitOn also hands CL to the frame slot, which resets it when that slot comes round. Resetting
        // it here as well would recycle one command buffer twice -- two callers then record into the same
        // VkCommandBuffer while it is pending, which is a device loss, not a warning.
        const uint64 Value = Core::SubmitOn(EQueueType::Graphics, TSpan{&CL, 1});
        Upload::NoteFlushSubmitted(Batch, 0, EQueueType::Graphics, Core::GetQueueTimeline(EQueueType::Graphics), Value);

        WaitSemaphore(Core::GetQueueTimeline(EQueueType::Graphics), Value);

        for (GPUPtr Staging : OwnedStaging)
        {
            Free(Staging);
        }
    }

    namespace Upload
    {
        void Initialize()
        {
            GStagingSliceSize = ClampCPUWriteSlice("Staging", kStagingSliceRequest, kFramesInFlight);

            for (FStagingSlice& Slice : GUpload.Slices)
            {
                Slice.Gpu      = Malloc(GStagingSliceSize, kDefaultAlign, EMemoryType::CPUWrite);
                SetDebugName(Slice.Gpu, "Upload.StagingSlice");
                Slice.Cpu      = static_cast<std::byte*>(ToHost(Slice.Gpu));
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
                Slice.Gpu      = 0;
                Slice.Cpu      = nullptr;
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

        struct FFlushTarget
        {
            FCmdListH          CL        = {};
            uint32             SliceMask = 0;
            bool               bAny      = false;
            TVector<FTextureH> WrittenTextures;

            struct FWrittenRange { GPUPtr Begin; GPUPtr End; };
            TVector<FWrittenRange> WrittenBuffers;

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
                          TVector<GPUPtr>& OutOwnedStaging, uint64* OutBatch)
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
                GUpload.QueuedOps.store(0u, std::memory_order_relaxed);

                // Closes the batch these ops belonged to and opens the next one. Under the queue lock, so
                // BatchForQueuedOps can never hand out a batch whose ops have already gone out.
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

                // A copy READS a texture as well as writing one, so an earlier write to its source in this
                // same flush has to be ordered against too -- otherwise it copies pre-upload contents.
                const bool bReadsWritten = Op.Type == EUploadOp::TextureCopy && T.AlreadyWritten(Op.TextureSource);

                if (bReadsWritten
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
                if (Op.bOwnedStaging)
                {
                    OutOwnedStaging.push_back(Op.Staging);
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
            return Result;
        }

        bool Flush(FCmdListH CL, TVector<GPUPtr>& OutOwnedStaging, uint32* OutSliceMask, uint64* OutBatch)
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
            TVector<GPUPtr> Orphaned;
            {
                FScopeLock Lock(GUpload.Mutex);

                size_t Write = 0;
                for (size_t Read = 0; Read < GUpload.Queue.size(); ++Read)
                {
                    FUploadOp& Op = GUpload.Queue[Read];
                    if (Targets(Op))
                    {
                        if (Op.bOwnedStaging)
                        {
                            Orphaned.push_back(Op.Staging);
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
            
            for (GPUPtr Staging : Orphaned)
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

            // Source as well as destination: a queued copy READING an image that is about to be destroyed
            // would record against a dead VkImage exactly as a write would.
            CancelMatching([Texture](const FUploadOp& Op)
            {
                return Op.Type != EUploadOp::Buffer
                    && (Op.TextureDest.Handle == Texture.Handle || Op.TextureSource.Handle == Texture.Handle);
            });
        }

        void CancelBuffer(GPUPtr Dest)
        {
            if (!GUpload.bInitialized || Dest == 0 || GUpload.QueuedOps.load(std::memory_order_relaxed) == 0u)
            {
                return;
            }
            
            GPUPtr Base = 0;
            uint64 Size = 0;
            if (!GetAllocationRange(Dest, Base, Size))
            {
                return;
            }

            const GPUPtr End = Base + Size;
            
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

            // A flush can straddle two queues (buffers on transfer, images on graphics); the gate closes
            // only when BOTH have signalled, which is why this accumulates rather than overwrites.
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

            // Nothing queued means everything queued has already been swept out, and the newest sweep is
            // the one that took it. Returning the open batch there would wait on a flush that may never
            // happen -- no ops, no flush.
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

            // Batches are submitted in order on each queue, so the first one still running blocks the rest
            // and there is no reason to look past it.
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

                // No queue recorded means the flush has not been submitted yet -- the copies have not been
                // issued, let alone run.
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
                    // A BLOCKING wait on the frame thread for last frame's copies out of this slice. If a
                    // hitch lands here the upload itself was fine and the frame simply caught up with it.
                    LUMINA_PROFILE_SECTION("Upload::BeginSlot Wait");
                    WaitSemaphore(Slice.ReadSemaphore[QueueIndex], Gate);
                }
                Slice.ReadValue[QueueIndex] = 0;
            }

            GPUPtr OldGpu = 0;
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
                        NewCapacity     = Math::Max(GStagingSliceSize, Math::AlignUp(Demand + Demand / 2, 1024ull * 1024));
                        Slice.LowStreak = 0;
                    }
                }
                else
                {
                    Slice.LowStreak = 0;
                }

                if (NewCapacity != Slice.Capacity)
                {
                    const GPUPtr NewGpu = Malloc(NewCapacity, kDefaultAlign, EMemoryType::CPUWrite);
                    if (NewGpu != 0)
                    {
                        SetDebugName(NewGpu, "Upload.StagingSlice");
                        OldGpu         = Slice.Gpu;
                        Slice.Gpu      = NewGpu;
                        Slice.Cpu      = static_cast<std::byte*>(ToHost(NewGpu));
                        Slice.Capacity = NewCapacity;
                        Slice.bWarnedGrowFailed = false;
                    }
                    else if (NewCapacity > Slice.Capacity && !Slice.bWarnedGrowFailed)
                    {
                        // Silent before this: the ring simply kept its old capacity and every oversized
                        // upload took the dedicated-allocation arm of ReserveLocked -- a fresh
                        // vkAllocateMemory + map on the calling thread, EVERY frame, for as long as demand
                        // stayed high. That reads as "staging a mip is slow" and is really "the ring is
                        // too small and cannot get bigger".
                        Slice.bWarnedGrowFailed = true;
                        LOG_WARN("RHI: upload staging slice could not grow {} -> {} MiB (CPU-visible VRAM "
                                 "exhausted). Uploads larger than the slice will allocate dedicated staging "
                                 "on the calling thread until demand drops.",
                            Slice.Capacity >> 20, NewCapacity >> 20);
                    }
                }

                // The number that explains a slow StageTextureMip: non-zero means uploads are falling out
                // of the ring and paying for their own allocation.
                LUMINA_PROFILE_VALUE("Upload/StagingOverflowKiB", (int64)(Slice.OverflowBytes / 1024));
                LUMINA_PROFILE_VALUE("Upload/StagingUsedKiB",     (int64)(Slice.Cursor / 1024));

                GUpload.CurrentSlot   = Slot;
                Slice.Cursor          = 0;
                Slice.OverflowBytes   = 0;
            }

            // Outside the lock: Core::Retire() re-enters Upload::CancelBuffer(), which takes GUpload.Mutex.
            Core::Retire(OldGpu);
        }
    }
}
