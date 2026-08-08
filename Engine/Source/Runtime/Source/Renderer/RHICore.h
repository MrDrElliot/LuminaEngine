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

        RUNTIME_API void DeferredFree(GPUPtr Memory, uint32 ExtraFrames = 0);

        FPipelineH CreateGraphicsPipeline(const FName& VertexShader, const FName& PixelShader, const FRasterDesc& Desc);
        FPipelineH CreateComputePipeline(const FName& ComputeShader);
    }
}
