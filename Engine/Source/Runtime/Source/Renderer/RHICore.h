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
        // Exported so a host without the engine's frame loop -- RHITests, a dedicated server -- can bring
        // the RHI up and drive the frame ring itself.
        RUNTIME_API void Initialize();
        RUNTIME_API void Shutdown();

        RUNTIME_API void BeginFrame(uint32 SlotIndex);

        void Submit(FCmdListH CommandList);

        RUNTIME_API uint64 SubmitOn(EQueueType Queue, TSpan<const FCmdListH> CommandLists, TSpan<const FSemaphoreInfo> Waits = {});

        RUNTIME_API FSemaphoreH GetQueueTimeline(EQueueType Queue);

        void SubmitAndWait(FCmdListH CommandList);

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

        template<typename T>
        GPUPtr CopyTransientArray(const T* Data, uint64 Count)
        {
            FTransientAlloc Alloc = AllocTransient(sizeof(T) * Count, alignof(T) > kDefaultAlign ? alignof(T) : kDefaultAlign);
            Memory::Memcpy(Alloc.Cpu, Data, sizeof(T) * Count);
            return Alloc.Gpu;
        }
        
        RUNTIME_API void Retire(GPUPtr Memory, uint32 ExtraCycles = 0);
        RUNTIME_API void Retire(FTextureH Texture);
        RUNTIME_API void Retire(FPipelineH Pipeline);
        RUNTIME_API void RetireSampledSlot(uint32 HeapSlot);
        RUNTIME_API void RetireStorageSlot(uint32 HeapSlot);

        FPipelineH CreateGraphicsPipeline(const FName& VertexShader, const FName& PixelShader, const FRasterDesc& Desc);
        FPipelineH CreateComputePipeline(const FName& ComputeShader);
    }
}
