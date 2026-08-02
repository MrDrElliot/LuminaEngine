#include "RuntimePCH.h"
#include "RHIUpload.h"
#include "RHICore.h"

#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"
#include "Memory/Memcpy.h"

namespace Lumina::RHI
{
    namespace
    {
        // Per-frame-in-flight staging slice. Sized for a steady-state burst; an upload
        // larger than the slice's remaining space falls back to a dedicated (deferred-
        // freed) allocation, so this never has to fit a worst-case load spike.
        constexpr uint64 kStagingSliceSize = 64ull * 1024 * 1024;

        enum class EUploadOp : uint8 { Buffer, Texture, Clear };

        struct FUploadOp
        {
            EUploadOp   Type;
            GPUPtr      Staging        = 0;     // source for Buffer/Texture, 0 for Clear
            bool        bOwnedStaging  = false; // true -> DeferredFree after the copy retires
            GPUPtr      BufferDest     = 0;     // Buffer
            FTextureH   TextureDest    = {};    // Texture/Clear
            uint64      Size           = 0;
            uint32      RowPitchTexels = 0;     // Texture
            uint32      Mip            = 0;     // Texture
            float       ClearValue[4]  = {};    // Clear
        };

        struct FStagingSlice
        {
            GPUPtr      Gpu    = 0;
            std::byte*  Cpu    = nullptr;
            uint64      Cursor = 0;
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

        struct FStaging { std::byte* Cpu; GPUPtr Gpu; bool bOwned; };

        // Reserve Size bytes of CPU-write staging from the current slice; falls back to a
        // dedicated allocation when the slice is full. Caller memcpys outside the lock.
        FStaging ReserveLocked(uint64 Size, uint64 Alignment)
        {
            FStagingSlice& Slice = GUpload.Slices[GUpload.CurrentSlot];
            const uint64 Aligned = Math::AlignUp(Slice.Cursor, Alignment);
            if (Aligned + Size <= kStagingSliceSize)
            {
                Slice.Cursor = Aligned + Size;
                return { Slice.Cpu + Aligned, Slice.Gpu + Aligned, false };
            }

            const GPUPtr Owned = Malloc(Size, Alignment, EMemoryType::CPUWrite);
            return { static_cast<std::byte*>(ToHost(Owned)), Owned, true };
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
        Memory::Memcpy(S.Cpu, Data, Size);

        FUploadOp Op;
        Op.Type          = EUploadOp::Buffer;
        Op.Staging       = S.Gpu;
        Op.bOwnedStaging = S.bOwned;
        Op.BufferDest    = Dest;
        Op.Size          = Size;

        FScopeLock Lock(GUpload.Mutex);
        GUpload.Queue.push_back(Op);
    }

    void UploadTexture(FTextureH Dest, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels)
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
        Memory::Memcpy(S.Cpu, Data, Size);

        FUploadOp Op;
        Op.Type           = EUploadOp::Texture;
        Op.Staging        = S.Gpu;
        Op.bOwnedStaging  = S.bOwned;
        Op.TextureDest    = Dest;
        Op.Size           = Size;
        Op.RowPitchTexels = RowPitchTexels;
        Op.Mip            = Mip;

        FScopeLock Lock(GUpload.Mutex);
        GUpload.Queue.push_back(Op);
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

        const uint64 Value = ++GUpload.FlushCounter;
        const FSemaphoreInfo Signal { GUpload.FlushSemaphore, Value, EStageFlags::AllCommands };
        Submit(EQueueType::Graphics, TSpan{&CL, 1}, {}, TSpan{&Signal, 1});

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
                Slice = FStagingSlice{};
            }

            FreeH(GUpload.FlushSemaphore);
            GUpload.bInitialized = false;
        }

        bool Flush(FCmdListH CL)
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

            for (const FUploadOp& Op : Ops)
            {
                const bool bWritesTexture = (Op.Type == EUploadOp::Texture || Op.Type == EUploadOp::Clear);
                if (bWritesTexture && AlreadyWritten(Op.TextureDest))
                {
                    Barriers::TransferToTransfer(CL);
                    WrittenTextures.clear();
                }

                switch (Op.Type)
                {
                case EUploadOp::Buffer:
                    CmdMemcpy(CL, Op.BufferDest, Op.Staging, Op.Size);
                    break;
                case EUploadOp::Texture:
                    {
                        FTextureSlice Slice;
                        Slice.Mip = Op.Mip;
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
            return true;
        }

        void BeginSlot(uint32 Slot)
        {
            FScopeLock Lock(GUpload.Mutex);
            GUpload.CurrentSlot           = Slot;
            GUpload.Slices[Slot].Cursor   = 0;
        }
    }
}
