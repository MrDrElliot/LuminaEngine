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

    static constexpr uint32 kNumQueues = 3;   // EQueueType::Graphics / Transfer / Compute

    // One retirement queue per frame slot. Kinds rather than a queue each, so there is exactly one place
    // in the engine that destroys a GPU resource and one invariant to check.
    struct FRetireItem
    {
        enum class EKind : uint8 { Buffer, Texture, SampledSlot, StorageSlot, Pipeline };

        EKind      Kind        = EKind::Buffer;
        uint32     ExtraCycles = 0;
        GPUPtr     Memory      = 0;
        FTextureH  Texture     = {};
        uint32     Slot        = kInvalidHeapSlot;
        FPipelineH Pipeline    = {};

        /** Per-queue timeline value this resource must outlive: the value the NEXT submission on that queue
         *  will signal, captured at retire time. Timelines are monotonic, so waiting it covers everything
         *  already submitted AND the one command list that may have been mid-record when the retire landed.
         *  The frame slot alone cannot express this -- its wait value is the frame from two frames back. */
        uint64     Fence[kNumQueues] = {};
    };

    struct FCoreState
    {
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

        TVector<FRetireItem> RetireQueues[kFramesInFlight];
        FMutex               RetireMutex;

        bool                bInitialized = false;
    };

    static FCoreState GCore;

    static void DestroyRetired(const FRetireItem& Item);

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

        // Device is idle, so every hold has expired by definition: destroy both slots outright, ignoring
        // ExtraCycles. Runs before the heap is freed below, so slot retirements still resolve.
        {
            FScopeLock Lock(GCore.RetireMutex);
            for (TVector<FRetireItem>& Queue : GCore.RetireQueues)
            {
                for (const FRetireItem& Item : Queue)
                {
                    DestroyRetired(Item);
                }
                Queue.clear();
            }
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

    // The single point in the engine that destroys a GPU resource.
    static void DestroyRetired(const FRetireItem& Item)
    {
        switch (Item.Kind)
        {
        case FRetireItem::EKind::Buffer:
            RHI::Free(Item.Memory);
            break;
        case FRetireItem::EKind::Texture:
            RHI::FreeH(Item.Texture);
            break;
        case FRetireItem::EKind::SampledSlot:
            // After Shutdown the heap itself is gone, so its slots died with it.
            if (GCore.bInitialized) { RHI::HeapFreeTexture(GCore.GlobalHeap, Item.Slot); }
            break;
        case FRetireItem::EKind::StorageSlot:
            if (GCore.bInitialized) { RHI::HeapFreeRWTexture(GCore.GlobalHeap, Item.Slot); }
            break;
        case FRetireItem::EKind::Pipeline:
            // RHI::FreeH calls vkDestroyPipeline SYNCHRONOUSLY, so this is the only safe way to drop a
            // pipeline while frames are in flight -- a command buffer recorded two frames ago may still
            // reference it. Freeing one directly is only correct after a device wait-idle.
            RHI::FreeH(Item.Pipeline);
            break;
        }
    }

    static void PushRetire(FRetireItem Item)
    {
        // Asset destructors can outlive Core::Shutdown. With no frames in flight there is nothing to wait
        // for, so destroy immediately rather than queueing into a drain that will never run again.
        if (!GCore.bInitialized)
        {
            DestroyRetired(Item);
            return;
        }

        {
            FScopeLock Lock(GCore.SubmitMutex);
            for (uint32 QueueIndex = 0; QueueIndex < kNumQueues; ++QueueIndex)
            {
                Item.Fence[QueueIndex] = GCore.QueueCounter[QueueIndex] + 1;
            }
        }

        const uint32 Slot = GCore.CurrentSlot.load(std::memory_order_acquire);

        FScopeLock Lock(GCore.RetireMutex);
        GCore.RetireQueues[Slot].push_back(Item);
    }

    // Called from BeginFrame once this slot's queue timelines have been waited. The slot decides WHEN an
    // item is looked at; whether it may be destroyed is the per-item fence, because the slot wait only
    // covers the frame from two frames back -- the one in the other slot is still executing.
    static void DrainRetireQueue(uint32 Slot)
    {
        uint64 Submitted[kNumQueues];
        {
            FScopeLock Lock(GCore.SubmitMutex);
            for (uint32 QueueIndex = 0; QueueIndex < kNumQueues; ++QueueIndex)
            {
                Submitted[QueueIndex] = GCore.QueueCounter[QueueIndex];
            }
        }

        uint64 Signalled[kNumQueues];
        for (uint32 QueueIndex = 0; QueueIndex < kNumQueues; ++QueueIndex)
        {
            Signalled[QueueIndex] = GetSemaphoreValue(GCore.QueueTimeline[QueueIndex]);
        }

        // Clamped to what has actually been submitted: a queue that saw no submission since the retire has
        // no post-retire work to wait on, only the in-flight work at Fence-1. Without the clamp an idle
        // queue -- async compute during a quiet stretch, transfer with no uploads -- would pin the item
        // against a value nothing is ever going to signal.
        auto HasRetired = [&](const FRetireItem& Item)
        {
            for (uint32 QueueIndex = 0; QueueIndex < kNumQueues; ++QueueIndex)
            {
                const uint64 Needed = Math::Min(Item.Fence[QueueIndex], Submitted[QueueIndex]);
                if (Signalled[QueueIndex] < Needed)
                {
                    return false;
                }
            }
            return true;
        };

        TVector<FRetireItem> Ready;
        {
            FScopeLock Lock(GCore.RetireMutex);
            TVector<FRetireItem>& Queue = GCore.RetireQueues[Slot];
            for (size_t i = 0; i < Queue.size(); )
            {
                if (Queue[i].ExtraCycles > 0)
                {
                    --Queue[i].ExtraCycles;
                    ++i;
                    continue;
                }
                // Not yet retired on some queue: leave it for the next drain of this slot.
                if (!HasRetired(Queue[i]))
                {
                    ++i;
                    continue;
                }
                Ready.push_back(Queue[i]);
                Queue[i] = Queue.back();
                Queue.pop_back();
            }
        }

        // Outside the lock: the destroy paths take their own backend locks.
        for (const FRetireItem& Item : Ready)
        {
            DestroyRetired(Item);
        }
    }

    void Retire(GPUPtr Memory, uint32 ExtraCycles)
    {
        if (Memory == 0)
        {
            return;
        }

        FRetireItem Item;
        Item.Kind        = FRetireItem::EKind::Buffer;
        Item.Memory      = Memory;
        Item.ExtraCycles = ExtraCycles;
        PushRetire(Item);
    }

    void Retire(FPipelineH Pipeline)
    {
        if (!IsValid(Pipeline))
        {
            return;
        }

        FRetireItem Item;
        Item.Kind     = FRetireItem::EKind::Pipeline;
        Item.Pipeline = Pipeline;
        PushRetire(Item);
    }

    void Retire(FTextureH Texture)
    {
        if (!IsValid(Texture))
        {
            return;
        }

        FRetireItem Item;
        Item.Kind    = FRetireItem::EKind::Texture;
        Item.Texture = Texture;
        PushRetire(Item);
    }

    void RetireSampledSlot(uint32 HeapSlot)
    {
        if (HeapSlot == kInvalidHeapSlot)
        {
            return;
        }

        // Unbind NOW, free the index at the drain. Deferring both would leave the descriptor pointing at the
        // texture for the whole retire->drain window, so every frame recorded in it binds a texture the
        // caller has already given up -- and the drain only waits ITS slot's timeline, which leaves the
        // other slot's frame in flight when the image is destroyed. Repointing mid-flight is what the
        // heap's UPDATE_AFTER_BIND flags are for, and is what HeapRepointTexture already does.
        if (GCore.bInitialized)
        {
            RHI::HeapUnbindTexture(GCore.GlobalHeap, HeapSlot);
        }

        FRetireItem Item;
        Item.Kind = FRetireItem::EKind::SampledSlot;
        Item.Slot = HeapSlot;
        PushRetire(Item);
    }

    void RetireStorageSlot(uint32 HeapSlot)
    {
        if (HeapSlot == kInvalidHeapSlot)
        {
            return;
        }

        FRetireItem Item;
        Item.Kind = FRetireItem::EKind::StorageSlot;
        Item.Slot = HeapSlot;
        PushRetire(Item);
    }

    void BeginFrame(uint32 SlotIndex)
    {
        if (!GCore.bInitialized)
        {
            return;
        }

        const uint32 Slot = SlotIndex % kFramesInFlight;

        // Every queue that submitted into this slot, not just the last one to do so.
        for (uint32 QueueIndex = 0; QueueIndex < kNumQueues; ++QueueIndex)
        {
            const uint64 WaitValue = GCore.SlotWaitValue[Slot][QueueIndex];
            if (WaitValue != 0)
            {
                WaitSemaphore(GCore.QueueTimeline[QueueIndex], WaitValue);
                GCore.SlotWaitValue[Slot][QueueIndex] = 0;
            }
        }

        // The waits above are the entire lifetime contract: everything recorded into this slot has now
        // retired on the GPU. Destroy this slot's retired resources here, and nowhere else.
        DrainRetireQueue(Slot);
        RHI::RetireSlot(Slot);

        // Shader-library entries whose last owning material dropped them. Freed HERE and only here: handles
        // are dereferenced lock-free off the render thread, so recycling a slot is only safe at a point
        // where no lookup can be in flight. The generation bump is what makes every weak FShaderH still
        // pointing at the entry start resolving to null, so its holder re-resolves instead of faulting.
        FShaderLibrary::FlushPendingReleases();

        Upload::DrainSliceWriters(Slot);

        for (FCmdListH CommandList : GCore.SlotCommandLists[Slot])
        {
            ResetCommandList(CommandList);
        }
        GCore.SlotCommandLists[Slot].clear();

        // Dedicated staging blocks the flush below reads. Released after CurrentSlot becomes Slot, so they
        // land on THIS slot's retire queue -- the one gated by the upload submit's own timeline value.
        TVector<GPUPtr> UploadStaging;

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
            const uint32 Used = Upload::FlushSplit(BufferCL, ImageCL, &BufferSlices, &ImageSlices, UploadStaging);

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
                Retire(Slice.Gpu);
                Slice.Gpu = Malloc(NewCapacity, kDefaultAlign, EMemoryType::CPUWrite);
                Slice.Cpu = static_cast<std::byte*>(ToHost(Slice.Gpu));
                Slice.Capacity = NewCapacity;
            }

            Slice.Cursor.store(0, std::memory_order_relaxed);
        }

        GCore.CurrentSlot.store(Slot, std::memory_order_release);

        // Only with CurrentSlot == Slot does Retire queue onto the slot whose SlotWaitValue the upload just
        // set. Retiring earlier queues on the previous slot, which waits a value older than that submit.
        for (GPUPtr Staging : UploadStaging)
        {
            Retire(Staging);
        }

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
            Retire(Mem);
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
        const FShaderEntry* Vertex = FShaderLibrary::Resolve(FShaderLibrary::Get(VertexShader));
        const FShaderEntry* Pixel   = FShaderLibrary::Resolve(FShaderLibrary::Get(PixelShader));

        if (Vertex == nullptr || !Vertex->IsValid() || Pixel == nullptr || !Pixel->IsValid())
        {
            LOG_ERROR("RHICore: missing shaders for pipeline ({} / {})", VertexShader.c_str(), PixelShader.c_str());
            return {};
        }

        return RHI::CreateGraphicsPipeline(Vertex->Source(), Pixel->Source(), Desc);
    }

    FPipelineH CreateComputePipeline(const FName& ComputeShader)
    {
        const FShaderEntry* Compute = FShaderLibrary::Resolve(FShaderLibrary::Get(ComputeShader));

        if (Compute == nullptr || !Compute->IsValid())
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
