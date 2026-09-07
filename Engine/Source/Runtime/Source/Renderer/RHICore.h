#pragma once

#include "RHI.h"
#include "RHIUpload.h"
#include "RenderResource.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Memory/Memcpy.h"

namespace Lumina::RHI
{
    struct FTransientAlloc
    {
        void*  Cpu  = nullptr;
        GPUPtr Gpu  = 0;
        uint64 Size = 0;

        operator FGPURange() const { return { Gpu, Size }; }
    };

    enum class EStockSampler : uint32
    {
        LinearWrap = 0,
        LinearClamp,
        LinearMirror,
        PointWrap,
        PointClamp,
        AnisoWrap,
        AnisoClamp,
        Shadow,
        MinReduction,
        MaxReduction,

        // Appended, not grouped: the slot index IS the value, and GlobalRHI.slang hardcodes it.
        PointMirror,
        AnisoMirror,

        Count
    };

    // Exported so a host without the engine's frame loop, RHITests or a dedicated server, can drive the ring.
    RUNTIME_API void BeginFrame(uint32 SlotIndex);

    /** The one submit. Every submission rides its queue's timeline and the returned value is what it signals,
     *  so waiting on it is WaitSemaphore(GetQueueTimeline(Queue), Value). The frame ring recycles the lists,
     *  so a caller must never reset one it submitted. */
    RUNTIME_API uint64 Submit(EQueueType Queue, TSpan<const FCmdListH> CommandLists, TSpan<const FSemaphoreInfo> Waits = {});

    RUNTIME_API FSemaphoreH GetQueueTimeline(EQueueType Queue);

    // Unexported on purpose, since FSwapchainTarget is what handles a rejected present.
    bool Present(FSwapchainH Swapchain, FCmdListH FinalCommandList);

    RUNTIME_API FTextureHeapH GetGlobalHeap();
    
    RUNTIME_API FTransientAlloc AllocTransient(uint64 Size, uint64 Alignment = kDefaultAlign);

    template<typename T>
    GPUPtr CopyTransient(const T& Value)
    {
        FTransientAlloc Alloc = AllocTransient(sizeof(T), alignof(T) > kDefaultAlign ? alignof(T) : kDefaultAlign);
        Memory::Memcpy(Alloc.Cpu, &Value, sizeof(T));
        return Alloc.Gpu;
    }

    // Returns a range, since the byte count is exactly what was copied.
    template<typename T>
    FGPURange CopyTransientArray(const T* Data, uint64 Count)
    {
        const uint64 Bytes = sizeof(T) * Count;
        FTransientAlloc Alloc = AllocTransient(Bytes, alignof(T) > kDefaultAlign ? alignof(T) : kDefaultAlign);
        Memory::Memcpy(Alloc.Cpu, Data, Bytes);
        return { Alloc.Gpu, Bytes };
    }
    
    RUNTIME_API void Retire(const FGPUAllocation& Memory);
    RUNTIME_API void Retire(FTextureH Texture);
    RUNTIME_API void Retire(FPipelineH Pipeline);
    RUNTIME_API void Retire(FSemaphoreH Semaphore);
    RUNTIME_API void Retire(FTextureHeapH Heap);
    RUNTIME_API void Retire(FSwapchainH Swapchain);
    RUNTIME_API void Retire(FSurfaceH Surface);
#if defined(LUMINA_WITH_GPU_PROFILING)
    RUNTIME_API void Retire(FQueryPoolH Pool);
#endif
    RUNTIME_API void RetireSampledSlot(uint32 HeapSlot);
    RUNTIME_API void RetireStorageSlot(uint32 HeapSlot);

    /** Runs Callback on the fence boundary a buffer retired at the same moment would be freed on.
     *
     *  For CPU-side state that DESCRIBES a GPU resource and has to stop describing it at exactly the
     *  instant it dies: any earlier and frames already recorded lose the resource they were built
     *  against, any later and frames recorded since read it after the free. A frame count cannot
     *  express that; the fence already does. */
    RUNTIME_API void RetireCallback(TFunction<void()> Callback);

    FPipelineH CreateGraphicsPipeline(const FName& VertexShader, const FName& PixelShader, const FRasterDesc& Desc);
    FPipelineH CreateComputePipeline(const FName& ComputeShader);

    /** Writes the driver's disassembly for Pipeline beside the log, when -dumpshaderisa is set. */
    RUNTIME_API void DumpPipelineISA(FPipelineH Pipeline, const FName& Name);
}
