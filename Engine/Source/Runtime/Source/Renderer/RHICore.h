#pragma once

#include "RHI.h"
#include "RHIUpload.h"
#include "RenderResource.h"
#include "Containers/Name.h"
#include "Memory/Memcpy.h"

namespace Lumina::RHI
{
    struct FTransientAlloc
    {
        void*  Cpu = nullptr;
        GPUPtr Gpu = 0;
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

        Count
    };

    namespace Core
    {
        void Initialize();
        void Shutdown();

        void BeginFrame(uint32 SlotIndex);

        void Submit(FCmdListH CommandList);

        RUNTIME_API uint64 SubmitOn(EQueueType Queue, TSpan<const FCmdListH> CommandLists, TSpan<const FSemaphoreInfo> Waits = {});

        RUNTIME_API FSemaphoreH GetQueueTimeline(EQueueType Queue);

        void SubmitAndWait(FCmdListH CommandList);

        bool Present(FSwapchainH Swapchain, FCmdListH FinalCommandList);

        RUNTIME_API FTextureHeapH GetGlobalHeap();

        FTransientAlloc AllocTransient(uint64 Size, uint64 Alignment = kDefaultAlign);

        template<typename T>
        GPUPtr CopyTransient(const T& Value)
        {
            FTransientAlloc Alloc = AllocTransient(sizeof(T), alignof(T) > kDefaultAlign ? alignof(T) : kDefaultAlign);
            Memory::Memcpy(Alloc.Cpu, &Value, sizeof(T));
            return Alloc.Gpu;
        }

        template<typename T>
        GPUPtr CopyTransientArray(const T* Data, uint64 Count)
        {
            FTransientAlloc Alloc = AllocTransient(sizeof(T) * Count, alignof(T) > kDefaultAlign ? alignof(T) : kDefaultAlign);
            Memory::Memcpy(Alloc.Cpu, Data, sizeof(T) * Count);
            return Alloc.Gpu;
        }

        // Resource retirement. A retired resource is destroyed at the top of the next BeginFrame for the
        // slot it was retired in, immediately after that slot's queue timelines have been waited -- which
        // is the exact point the GPU is known to be finished with it. Nothing here counts frames.
        //
        // Callable from any thread: items land in the slot currently being recorded, and BeginFrame
        // publishes CurrentSlot only AFTER draining, so nothing can be dropped into a list mid-drain.
        //
        // ExtraCycles holds the allocation for additional full slot rotations beyond GPU retirement. Only
        // the mesh-buffer path needs it, to outlive CPU-side memos of the meshlet header address; that is
        // a heuristic standing in for an ownership relationship, not a GPU lifetime.
        RUNTIME_API void Retire(GPUPtr Memory, uint32 ExtraCycles = 0);
        RUNTIME_API void Retire(FTextureH Texture);
        RUNTIME_API void RetireSampledSlot(uint32 HeapSlot);
        RUNTIME_API void RetireStorageSlot(uint32 HeapSlot);

        FPipelineH CreateGraphicsPipeline(const FName& VertexShader, const FName& PixelShader, const FRasterDesc& Desc);
        FPipelineH CreateComputePipeline(const FName& ComputeShader);
    }
}
