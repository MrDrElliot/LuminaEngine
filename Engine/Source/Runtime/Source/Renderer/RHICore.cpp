#include "RuntimePCH.h"
#include "RHICore.h"
#include "Log/Log.h"

#include "RenderResource.h"
#include "RHITexture.h"
#include "ShaderLibrary.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"

namespace Lumina::RHI::Core
{
    static constexpr uint64 kTransientSliceRequest = 32ull * 1024 * 1024;

    static uint64 GTransientSliceSize = kTransientSliceRequest;

    struct FTransientSlice
    {
        GPUPtr          Gpu = 0;
        std::byte*      Cpu = nullptr;
        uint64          Capacity = 0;
        uint32          LowStreak = 0;   // consecutive frames demand stayed below half capacity
        TAtomic<uint64> Cursor{0};
    };

    struct FPendingFree
    {
        GPUPtr Memory;
        uint32 TicksRemaining;
    };

    struct FCoreState
    {
        static constexpr uint32 kNumQueues = 3;   // EQueueType::Graphics / Transfer / Compute

        FTextureHeapH       GlobalHeap;

        FSemaphoreH         QueueTimeline[kNumQueues];
        uint64              QueueCounter[kNumQueues] = {};
        // This slot's high-water mark on each queue, waited independently in BeginFrame.
        uint64              SlotWaitValue[kFramesInFlight][kNumQueues] = {};

        uint64              PendingTransferWait = 0;
        bool                bQueueTookTransferWait[kNumQueues] = {};
        TVector<FCmdListH>  SlotCommandLists[kFramesInFlight];
        FTransientSlice     Slices[kFramesInFlight];

        TAtomic<uint32>     CurrentSlot{0};
        FMutex              SubmitMutex;

        TVector<FPendingFree> PendingFrees;
        FMutex                PendingFreeMutex;

        bool                bInitialized = false;
    };

    static FCoreState GCore;

    void Initialize()
    {
        GCore.GlobalHeap = CreateTextureHeap(8192, 1024, 64);

        for (FSemaphoreH& Timeline : GCore.QueueTimeline)
        {
            Timeline = CreateTimelineSemaphore(0);
        }

        Upload::Initialize();

        GTransientSliceSize = ClampCPUWriteSlice("Transient", kTransientSliceRequest, kFramesInFlight);

        for (FTransientSlice& Slice : GCore.Slices)
        {
            Slice.Gpu = Malloc(GTransientSliceSize, kDefaultAlign, EMemoryType::CPUWrite);
            Slice.Cpu = static_cast<std::byte*>(ToHost(Slice.Gpu));
            Slice.Capacity = GTransientSliceSize;
            Slice.Cursor.store(0, std::memory_order_relaxed);
        }

        auto AddSampler = [](EStockSampler Expected, const FSamplerDesc& Desc)
        {
            const uint32 Slot = HeapWriteSampler(GCore.GlobalHeap, Desc);
            ASSERT(Slot == (uint32)Expected, "Stock sampler slot mismatch");
        };

        FSamplerDesc Linear{};

        FSamplerDesc Desc = Linear;
        AddSampler(EStockSampler::LinearWrap, Desc);

        Desc = Linear;
        Desc.AddressU = Desc.AddressV = Desc.AddressW = EAddressMode::ClampToEdge;
        AddSampler(EStockSampler::LinearClamp, Desc);

        Desc = Linear;
        Desc.AddressU = Desc.AddressV = Desc.AddressW = EAddressMode::MirroredRepeat;
        AddSampler(EStockSampler::LinearMirror, Desc);

        Desc = Linear;
        Desc.MinFilter = Desc.MagFilter = Desc.MipFilter = EFilter::Nearest;
        AddSampler(EStockSampler::PointWrap, Desc);

        Desc.AddressU = Desc.AddressV = Desc.AddressW = EAddressMode::ClampToEdge;
        AddSampler(EStockSampler::PointClamp, Desc);

        Desc = Linear;
        Desc.MaxAnisotropy = 16.0f;
        AddSampler(EStockSampler::AnisoWrap, Desc);

        Desc.AddressU = Desc.AddressV = Desc.AddressW = EAddressMode::ClampToEdge;
        AddSampler(EStockSampler::AnisoClamp, Desc);

        Desc = Linear;
        Desc.AddressU = Desc.AddressV = Desc.AddressW = EAddressMode::ClampToEdge;
        Desc.CompareOp = EOp::Less;
        AddSampler(EStockSampler::Shadow, Desc);

        Desc = Linear;
        Desc.AddressU = Desc.AddressV = Desc.AddressW = EAddressMode::ClampToEdge;
        Desc.Reduction = EReduction::Min;
        AddSampler(EStockSampler::MinReduction, Desc);

        Desc.Reduction = EReduction::Max;
        AddSampler(EStockSampler::MaxReduction, Desc);

        GCore.bInitialized = true;

        Textures::Initialize();
    }

    void Shutdown()
    {
        if (!GCore.bInitialized)
        {
            return;
        }

        WaitDeviceIdle();

        Upload::Shutdown();
        Textures::Shutdown();

        // Device is idle: flush every deferred buffer free immediately.
        {
            FScopeLock Lock(GCore.PendingFreeMutex);
            for (const FPendingFree& Pending : GCore.PendingFrees)
            {
                RHI::Free(Pending.Memory);
            }
            GCore.PendingFrees.clear();
        }

        for (FTransientSlice& Slice : GCore.Slices)
        {
            Free(Slice.Gpu);
            Slice.Gpu = 0;
            Slice.Cpu = nullptr;
            Slice.Cursor.store(0, std::memory_order_relaxed);
        }

        for (TVector<FCmdListH>& Lists : GCore.SlotCommandLists)
        {
            Lists.clear();
        }

        for (FSemaphoreH& Timeline : GCore.QueueTimeline)
        {
            FreeH(Timeline);
        }
        FreeH(GCore.GlobalHeap);
        GCore.bInitialized = false;
    }

    void DeferredFree(GPUPtr Memory, uint32 ExtraFrames)
    {
        if (Memory == 0)
        {
            return;
        }

        if (!GCore.bInitialized)
        {
            RHI::Free(Memory);
            return;
        }

        FScopeLock Lock(GCore.PendingFreeMutex);
        GCore.PendingFrees.push_back(FPendingFree{ Memory, kFramesInFlight + ExtraFrames });
    }

    void BeginFrame(uint32 SlotIndex)
    {
        if (!GCore.bInitialized)
        {
            return;
        }

        const uint32 Slot = SlotIndex % kFramesInFlight;

        // Every queue that submitted into this slot, not just the last one to do so.
        for (uint32 QueueIndex = 0; QueueIndex < FCoreState::kNumQueues; ++QueueIndex)
        {
            const uint64 WaitValue = GCore.SlotWaitValue[Slot][QueueIndex];
            if (WaitValue != 0)
            {
                WaitSemaphore(GCore.QueueTimeline[QueueIndex], WaitValue);
                GCore.SlotWaitValue[Slot][QueueIndex] = 0;
            }
        }

        Upload::DrainSliceWriters(Slot);

        for (FCmdListH CommandList : GCore.SlotCommandLists[Slot])
        {
            ResetCommandList(CommandList);
        }
        GCore.SlotCommandLists[Slot].clear();

        Textures::Tick();
        RHI::TickFrame();

        {
            FScopeLock Lock(GCore.PendingFreeMutex);
            for (size_t i = 0; i < GCore.PendingFrees.size(); )
            {
                FPendingFree& Pending = GCore.PendingFrees[i];
                if (Pending.TicksRemaining > 0)
                {
                    --Pending.TicksRemaining;
                    ++i;
                    continue;
                }

                RHI::Free(Pending.Memory);
                GCore.PendingFrees[i] = GCore.PendingFrees.back();
                GCore.PendingFrees.pop_back();
            }
        }

        {
            GCore.PendingTransferWait = 0;
            for (bool& bTook : GCore.bQueueTookTransferWait)
            {
                bTook = false;
            }

            const bool bAsyncTransfer = SupportsAsyncTransfer();

            const FCmdListH ImageCL  = OpenCommandList(EQueueType::Graphics);
            const FCmdListH BufferCL = bAsyncTransfer ? OpenCommandList(EQueueType::Transfer) : ImageCL;

            uint32 BufferSlices = 0;
            uint32 ImageSlices  = 0;
            const uint32 Used = Upload::FlushSplit(BufferCL, ImageCL, &BufferSlices, &ImageSlices);

            auto SubmitUpload = [&](EQueueType Queue, FCmdListH CL, uint32 SliceMask) -> uint64
            {
                const uint32 QueueIndex = (uint32)Queue;

                FScopeLock Lock(GCore.SubmitMutex);
                const uint64 Value = ++GCore.QueueCounter[QueueIndex];
                const FSemaphoreInfo Signal { GCore.QueueTimeline[QueueIndex], Value, EStageFlags::AllCommands };
                RHI::Submit(Queue, TSpan{&CL, 1}, {}, TSpan{&Signal, 1});
                GCore.SlotWaitValue[Slot][QueueIndex] = Value;
                GCore.SlotCommandLists[Slot].push_back(CL);

                Upload::NoteFlushSubmitted(SliceMask, Queue, GCore.QueueTimeline[QueueIndex], Value);
                return Value;
            };

            if (bAsyncTransfer && (Used & 1u) != 0u)
            {
                GCore.PendingTransferWait = SubmitUpload(EQueueType::Transfer, BufferCL, BufferSlices);
            }
            else if (bAsyncTransfer)
            {
                ResetCommandList(BufferCL);
            }

            // When not split, ImageCL carries both halves and both slice masks apply to it.
            const uint32 GraphicsSlices = bAsyncTransfer ? ImageSlices : (BufferSlices | ImageSlices);
            const uint32 GraphicsBit    = bAsyncTransfer ? 2u : 3u;

            if ((Used & GraphicsBit) != 0u)
            {
                SubmitUpload(EQueueType::Graphics, ImageCL, GraphicsSlices);
            }
            else
            {
                ResetCommandList(ImageCL);
            }
        }

        {
            FTransientSlice& Slice = GCore.Slices[Slot];
            const uint64 Demand = Slice.Cursor.load(std::memory_order_relaxed);

            uint64 NewCapacity = Slice.Capacity;
            if (Demand > Slice.Capacity)
            {
                NewCapacity = Math::AlignUp(Demand + Demand / 2, 1024ull * 1024); // grow 1.5x, MiB-rounded
                Slice.LowStreak = 0;
            }
            else if (Slice.Capacity > GTransientSliceSize && Demand * 2 < Slice.Capacity && ++Slice.LowStreak >= 64)
            {
                NewCapacity = Math::Max(GTransientSliceSize, Math::AlignUp(Demand + Demand / 2, 1024ull * 1024));
                Slice.LowStreak = 0;
            }
            else if (Demand * 2 >= Slice.Capacity)
            {
                Slice.LowStreak = 0;
            }

            if (NewCapacity != Slice.Capacity)
            {
                DeferredFree(Slice.Gpu);
                Slice.Gpu = Malloc(NewCapacity, kDefaultAlign, EMemoryType::CPUWrite);
                Slice.Cpu = static_cast<std::byte*>(ToHost(Slice.Gpu));
                Slice.Capacity = NewCapacity;
            }

            Slice.Cursor.store(0, std::memory_order_relaxed);
        }

        GCore.CurrentSlot.store(Slot, std::memory_order_release);
        Upload::BeginSlot(Slot);
    }

    uint64 SubmitOn(EQueueType Queue, TSpan<const FCmdListH> CommandLists, TSpan<const FSemaphoreInfo> Waits)
    {
        const uint32 QueueIndex = (uint32)Queue;

        FScopeLock Lock(GCore.SubmitMutex);

        const uint64 Value = ++GCore.QueueCounter[QueueIndex];

        FSemaphoreInfo WaitStorage[8];
        TSpan<const FSemaphoreInfo> EffectiveWaits = Waits;
        if (Queue != EQueueType::Transfer
            && GCore.PendingTransferWait != 0
            && !GCore.bQueueTookTransferWait[QueueIndex]
            && Waits.size() < (SIZE_T)(sizeof(WaitStorage) / sizeof(WaitStorage[0])))
        {
            SIZE_T Count = 0;
            for (const FSemaphoreInfo& Wait : Waits)
            {
                WaitStorage[Count++] = Wait;
            }
            WaitStorage[Count++] = FSemaphoreInfo{ GCore.QueueTimeline[(uint32)EQueueType::Transfer],
                                                   GCore.PendingTransferWait, EStageFlags::AllCommands };

            EffectiveWaits = TSpan<const FSemaphoreInfo>(WaitStorage, Count);
            GCore.bQueueTookTransferWait[QueueIndex] = true;
        }

        const FSemaphoreInfo Signal { GCore.QueueTimeline[QueueIndex], Value, EStageFlags::AllCommands };
        RHI::Submit(Queue, CommandLists, EffectiveWaits, TSpan{&Signal, 1});

        // Relaxed: only BeginFrame writes this, and both run on the frame thread.
        const uint32 Slot = GCore.CurrentSlot.load(std::memory_order_relaxed);
        GCore.SlotWaitValue[Slot][QueueIndex] = Value;
        for (FCmdListH CommandList : CommandLists)
        {
            GCore.SlotCommandLists[Slot].push_back(CommandList);
        }

        return Value;
    }

    FSemaphoreH GetQueueTimeline(EQueueType Queue)
    {
        return GCore.QueueTimeline[(uint32)Queue];
    }

    void Submit(FCmdListH CommandList)
    {
        SubmitOn(EQueueType::Graphics, TSpan{&CommandList, 1}, {});
    }

    void SubmitAndWait(FCmdListH CommandList)
    {
        constexpr uint32 GraphicsIndex = (uint32)EQueueType::Graphics;

        uint64 Value;
        {
            FScopeLock Lock(GCore.SubmitMutex);
            Value = ++GCore.QueueCounter[GraphicsIndex];

            const FSemaphoreInfo Signal { GCore.QueueTimeline[GraphicsIndex], Value, EStageFlags::AllCommands };
            RHI::Submit(EQueueType::Graphics, TSpan{&CommandList, 1}, {}, TSpan{&Signal, 1});
        }

        WaitSemaphore(GCore.QueueTimeline[GraphicsIndex], Value);
    }

    bool Present(FSwapchainH Swapchain, FCmdListH CommandList)
    {
        // Present submits and presents on the graphics queue, so it rides that queue's timeline.
        constexpr uint32 GraphicsIndex = (uint32)EQueueType::Graphics;

        FScopeLock Lock(GCore.SubmitMutex);

        const uint64 Value = ++GCore.QueueCounter[GraphicsIndex];

        // PresentSwapchain submits CommandList (wait acquire, signal present + this frame value), presents.
        const bool bOk = PresentSwapchain(Swapchain, CommandList, GCore.QueueTimeline[GraphicsIndex], Value);

        const uint32 Slot = GCore.CurrentSlot.load(std::memory_order_relaxed);
        GCore.SlotWaitValue[Slot][GraphicsIndex] = Value;
        GCore.SlotCommandLists[Slot].push_back(CommandList);
        return bOk;
    }

    FTextureHeapH GetGlobalHeap()
    {
        return GCore.GlobalHeap;
    }

    FTransientAlloc AllocTransient(uint64 Size, uint64 Alignment)
    {
        FTransientSlice& Slice = GCore.Slices[GCore.CurrentSlot.load(std::memory_order_acquire)];

        const uint64 Padded = Size + Alignment;
        const uint64 RawOffset = Slice.Cursor.fetch_add(Padded, std::memory_order_relaxed);
        
        if (RawOffset + Padded > Slice.Capacity)
        {
            const GPUPtr Mem = Malloc(Size, Alignment, EMemoryType::CPUWrite);
            DeferredFree(Mem);
            return FTransientAlloc{ .Cpu = ToHost(Mem), .Gpu = Mem };
        }

        const uint64 AlignedGpu = Math::AlignUp(Slice.Gpu + RawOffset, Alignment);
        const uint64 Skew = AlignedGpu - (Slice.Gpu + RawOffset);

        return FTransientAlloc
        {
            .Cpu = Slice.Cpu + RawOffset + Skew,
            .Gpu = AlignedGpu
        };
    }

    FPipelineH CreateGraphicsPipeline(const FName& VertexShader, const FName& PixelShader, const FRasterDesc& Desc)
    {
        const FShaderEntry* Vertex = FShaderLibrary::Get(VertexShader);
        const FShaderEntry* Pixel  = FShaderLibrary::Get(PixelShader);

        if (!Vertex->IsValid() || !Pixel->IsValid())
        {
            LOG_ERROR("RHICore: missing shaders for pipeline ({} / {})", VertexShader.c_str(), PixelShader.c_str());
            return {};
        }

        return RHI::CreateGraphicsPipeline(Vertex->Source(), Pixel->Source(), Desc);
    }

    FPipelineH CreateComputePipeline(const FName& ComputeShader)
    {
        const FShaderEntry* Compute = FShaderLibrary::Get(ComputeShader);

        if (!Compute->IsValid())
        {
            LOG_ERROR("RHICore: missing compute shader {}", ComputeShader.c_str());
            return {};
        }

        return RHI::CreateComputePipeline(Compute->Source());
    }
}

namespace Lumina::RHI
{
    // Public entry point (declared RUNTIME_API in RHI.h); the timeline-fenced impl lives in Core.
    void SubmitAndWait(FCmdListH CommandList)
    {
        Core::SubmitAndWait(CommandList);
    }
}
