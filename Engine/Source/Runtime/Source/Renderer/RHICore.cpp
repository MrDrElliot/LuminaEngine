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
    // 32 MiB INITIAL per-frame slice. Grows on demand at BeginFrame when a frame's transient allocations
    // outgrow it (the spike frame is covered by a one-off fallback allocation), and shrinks back after
    // sustained low use so a brief spike (e.g. 5000 skinned meshes' bone matrices) doesn't permanently
    // reserve host memory.
    static constexpr uint64 kTransientSliceSize = 32ull * 1024 * 1024;

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
        FTextureHeapH       GlobalHeap;
        FSemaphoreH         FrameTimeline;
        uint64              TimelineCounter = 0;
        uint64              SlotWaitValue[kFramesInFlight] = {};
        TVector<FCmdListH>  SlotCommandLists[kFramesInFlight];
        FTransientSlice     Slices[kFramesInFlight];
        uint32              CurrentSlot = 0;
        FMutex              SubmitMutex;

        TVector<FPendingFree> PendingFrees;
        FMutex                PendingFreeMutex;

        bool                bInitialized = false;
    };

    static FCoreState GCore;

    void Initialize()
    {
        GCore.GlobalHeap      = CreateTextureHeap(8192, 1024, 64);
        GCore.FrameTimeline   = CreateTimelineSemaphore(0);

        Upload::Initialize();

        for (FTransientSlice& Slice : GCore.Slices)
        {
            Slice.Gpu = Malloc(kTransientSliceSize, kDefaultAlign, EMemoryType::CPUWrite);
            Slice.Cpu = static_cast<std::byte*>(ToHost(Slice.Gpu));
            Slice.Capacity = kTransientSliceSize;
            Slice.Cursor.store(0, std::memory_order_relaxed);
        }

        // Stock samplers. Registration order must match EStockSampler and the
        // SAMPLER_* constants in GlobalRHI.slang.
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

        FreeH(GCore.FrameTimeline);
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

        if (GCore.SlotWaitValue[Slot] != 0)
        {
            WaitSemaphore(GCore.FrameTimeline, GCore.SlotWaitValue[Slot]);
        }

        // Before anything destroys a resource this frame. Resetting a command list drops the
        // recorded references it holds;
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

        // Flush queued uploads as the frame's first GPU work, on the frame timeline so
        // slot recycling already waits for them. One submit + one Transfer->All barrier.
        {
            const FCmdListH UploadCL = OpenCommandList(EQueueType::Graphics);
            if (Upload::Flush(UploadCL))
            {
                FScopeLock Lock(GCore.SubmitMutex);
                const uint64 Value = ++GCore.TimelineCounter;
                const FSemaphoreInfo Signal { GCore.FrameTimeline, Value, EStageFlags::AllCommands };
                RHI::Submit(EQueueType::Graphics, TSpan{&UploadCL, 1}, {}, TSpan{&Signal, 1});
                GCore.SlotWaitValue[Slot] = Value;
                GCore.SlotCommandLists[Slot].push_back(UploadCL);
            }
            else
            {
                ResetCommandList(UploadCL);
            }
        }

        // Resize this slot's transient slice to last cycle's demand before resetting it. The slot's GPU work
        // has already retired (we waited its timeline above), so freeing/recreating the buffer is hazard-free.
        {
            FTransientSlice& Slice = GCore.Slices[Slot];
            const uint64 Demand = Slice.Cursor.load(std::memory_order_relaxed);

            uint64 NewCapacity = Slice.Capacity;
            if (Demand > Slice.Capacity)
            {
                NewCapacity = Math::AlignUp(Demand + Demand / 2, 1024ull * 1024); // grow 1.5x, MiB-rounded
                Slice.LowStreak = 0;
            }
            else if (Slice.Capacity > kTransientSliceSize && Demand * 2 < Slice.Capacity && ++Slice.LowStreak >= 64)
            {
                NewCapacity = Math::Max(kTransientSliceSize, Math::AlignUp(Demand + Demand / 2, 1024ull * 1024));
                Slice.LowStreak = 0;
            }
            else if (Demand * 2 >= Slice.Capacity)
            {
                Slice.LowStreak = 0;
            }

            if (NewCapacity != Slice.Capacity)
            {
                Free(Slice.Gpu);
                Slice.Gpu = Malloc(NewCapacity, kDefaultAlign, EMemoryType::CPUWrite);
                Slice.Cpu = static_cast<std::byte*>(ToHost(Slice.Gpu));
                Slice.Capacity = NewCapacity;
            }

            Slice.Cursor.store(0, std::memory_order_relaxed);
        }

        GCore.CurrentSlot = Slot;
        Upload::BeginSlot(Slot);
    }

    void Submit(FCmdListH CommandList)
    {
        FScopeLock Lock(GCore.SubmitMutex);

        const uint64 Value = ++GCore.TimelineCounter;

        const FSemaphoreInfo Signal { GCore.FrameTimeline, Value, EStageFlags::AllCommands };
        RHI::Submit(EQueueType::Graphics, TSpan{&CommandList, 1}, {}, TSpan{&Signal, 1});

        GCore.SlotWaitValue[GCore.CurrentSlot] = Value;
        GCore.SlotCommandLists[GCore.CurrentSlot].push_back(CommandList);
    }

    void SubmitAndWait(FCmdListH CommandList)
    {
        uint64 Value;
        {
            FScopeLock Lock(GCore.SubmitMutex);
            Value = ++GCore.TimelineCounter;

            const FSemaphoreInfo Signal { GCore.FrameTimeline, Value, EStageFlags::AllCommands };
            RHI::Submit(EQueueType::Graphics, TSpan{&CommandList, 1}, {}, TSpan{&Signal, 1});
        }

        // Wait for ONLY this submission's timeline value (not vkDeviceWaitIdle), which would block on
        // unrelated in-flight frame work. The caller owns CommandList and resets it after this returns,
        // so it is intentionally not tracked in the frame's slot lists.
        WaitSemaphore(GCore.FrameTimeline, Value);
    }

    bool Present(FSwapchainH Swapchain, FCmdListH CommandList)
    {
        FScopeLock Lock(GCore.SubmitMutex);

        const uint64 Value = ++GCore.TimelineCounter;

        // PresentSwapchain submits CommandList (wait acquire, signal present + this frame value), presents.
        const bool bOk = PresentSwapchain(Swapchain, CommandList, GCore.FrameTimeline, Value);

        GCore.SlotWaitValue[GCore.CurrentSlot] = Value;
        GCore.SlotCommandLists[GCore.CurrentSlot].push_back(CommandList);
        return bOk;
    }

    FTextureHeapH GetGlobalHeap()
    {
        return GCore.GlobalHeap;
    }

    FTransientAlloc AllocTransient(uint64 Size, uint64 Alignment)
    {
        FTransientSlice& Slice = GCore.Slices[GCore.CurrentSlot];

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
