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
        constexpr uint64 kStagingSliceRequest = 64ull * 1024 * 1024;

        // Resolved against the CPU-visible VRAM aperture in Initialize.
        uint64 GStagingSliceSize = kStagingSliceRequest;

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
            uint32      Width          = 0;         // Texture: mip extent, 0 = derive from the description
            uint32      Height         = 0;
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
        };

        struct FUploadState
        {
            FStagingSlice       Slices[kFramesInFlight];
            uint32              CurrentSlot = 0;
            TVector<FUploadOp>  Queue;
            FMutex              Mutex;

            FSemaphoreH         FlushSemaphore;
            uint64              FlushCounter = 0;
            
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
            GUpload.QueuedOps.store((uint32)GUpload.Queue.size(), std::memory_order_relaxed);
        }

        EndWrite(S);
    }

    void UploadTexture(FTextureH Dest, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels, uint32 Width, uint32 Height)
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
        Op.Width          = Width;
        Op.Height         = Height;

        {
            FScopeLock Lock(GUpload.Mutex);
            GUpload.Queue.push_back(Op);
            GUpload.QueuedOps.store((uint32)GUpload.Queue.size(), std::memory_order_relaxed);
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
        GUpload.QueuedOps.store((uint32)GUpload.Queue.size(), std::memory_order_relaxed);
    }

    void FlushUploadsAndWait()
    {
        if (!GUpload.bInitialized)
        {
            return;
        }

        TVector<GPUPtr> OwnedStaging;

        const FCmdListH CL = OpenCommandList(EQueueType::Graphics);
        if (!Upload::Flush(CL, OwnedStaging))
        {
            ResetCommandList(CL);
            return;
        }

        uint64 Value;
        {
            FScopeLock Lock(GUpload.Mutex);
            Value = ++GUpload.FlushCounter;

            const FSemaphoreInfo Signal { GUpload.FlushSemaphore, Value, EStageFlags::AllCommands };
            Submit(EQueueType::Graphics, TSpan{&CL, 1}, {}, TSpan{&Signal, 1});
        }

        WaitSemaphore(GUpload.FlushSemaphore, Value);
        ResetCommandList(CL);
        
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

            FreeH(GUpload.FlushSemaphore);
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
                          TVector<GPUPtr>& OutOwnedStaging)
        {
            TVector<FUploadOp> Ops;
            {
                FScopeLock Lock(GUpload.Mutex);
                if (GUpload.Queue.empty())
                {
                    return 0u;
                }
                Ops.swap(GUpload.Queue);
                GUpload.QueuedOps.store(0u, std::memory_order_relaxed);
            }

            const bool bSplit = (BufferCL.Handle != ImageCL.Handle);

            FFlushTarget Targets[2];
            Targets[0].CL = BufferCL;
            Targets[1].CL = ImageCL;

            for (const FUploadOp& Op : Ops)
            {
                const bool bWritesTexture = (Op.Type == EUploadOp::Texture || Op.Type == EUploadOp::Clear);
                const bool bWritesBuffer  = (Op.Type == EUploadOp::Buffer);

                FFlushTarget& T = (bSplit && bWritesBuffer) ? Targets[0] : Targets[1];
                T.bAny = true;

                if (Op.Slice != kNoSlice)
                {
                    T.SliceMask |= (1u << Op.Slice);
                }

                if ((bWritesTexture && T.AlreadyWritten(Op.TextureDest))
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

        bool Flush(FCmdListH CL, TVector<GPUPtr>& OutOwnedStaging, uint32* OutSliceMask)
        {
            uint32 BufferSlices = 0;
            uint32 ImageSlices  = 0;
            const uint32 Used = FlushSplit(CL, CL, &BufferSlices, &ImageSlices, OutOwnedStaging);

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

            CancelMatching([Texture](const FUploadOp& Op)
            {
                return Op.Type != EUploadOp::Buffer && Op.TextureDest.Handle == Texture.Handle;
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

        void NoteFlushSubmitted(uint32 SliceMask, EQueueType Queue, FSemaphoreH Semaphore, uint64 Value)
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
                else if (Slice.Capacity > GStagingSliceSize && Demand * 2 < Slice.Capacity && ++Slice.LowStreak >= 64)
                {
                    NewCapacity     = Math::Max(GStagingSliceSize, Math::AlignUp(Demand + Demand / 2, 1024ull * 1024));
                    Slice.LowStreak = 0;
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
                    }
                }

                GUpload.CurrentSlot   = Slot;
                Slice.Cursor          = 0;
                Slice.OverflowBytes   = 0;
            }

            // Outside the lock: Core::Retire() re-enters Upload::CancelBuffer(), which takes GUpload.Mutex.
            Core::Retire(OldGpu);
        }
    }
}
