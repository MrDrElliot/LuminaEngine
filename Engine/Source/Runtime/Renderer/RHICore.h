#pragma once

#include "RHI.h"
#include "RHIUpload.h"
#include "RenderResource.h"
#include "Containers/Name.h"
#include "Memory/Memcpy.h"

// Runtime support layer for the new RHI: the global texture heap, the per-frame
// transient ring, pipeline creation from the engine shader library, and the
// frame-in-flight sync that makes ResetCommandList / transient reuse safe.
// Initialized by FRenderManager right after RHI::CreateDevice.

namespace Lumina::RHI
{
    struct FTransientAlloc
    {
        void*  Cpu = nullptr;
        GPUPtr Gpu = 0;
    };

    // Heap slots of the always-present samplers, registered in this order by
    // Core::Initialize. Keep in lockstep with SAMPLER_* in GlobalRHI.slang.
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

        // Render thread. Waits for the GPU to finish this slot's previous frame,
        // recycles its command lists, and resets its transient ring slice.
        void BeginFrame(uint32 SlotIndex);

        // Render thread. Submits graphics work for the current slot and signals
        // the frame timeline so BeginFrame can pace slot reuse.
        void Submit(FCmdListH CommandList);

        // Submit + block until ONLY this submission completes (its own frame-timeline value), NOT the whole
        // device. Backs the public RHI::SubmitAndWait; see it for the deadlock rationale.
        void SubmitAndWait(FCmdListH CommandList);

        // Render thread. Presents FinalCommandList to the swapchain: signals the
        // frame timeline (paces BeginFrame) + recycles the list with the slot.
        bool Present(FSwapchainH Swapchain, FCmdListH FinalCommandList);

        RUNTIME_API FTextureHeapH GetGlobalHeap();

        // Transient per-frame GPU memory (CPU-write, device-addressable). Valid
        // until the slot is reused. Thread-safe (atomic bump).
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

        // Frees the memory once every in-flight frame has retired. Thread-safe.
        RUNTIME_API void DeferredFree(GPUPtr Memory);

        // Pipelines from the engine shader library ("MyShader.slang" keys); the
        // library compiles/caches through the existing slang path.
        FPipelineH CreateGraphicsPipeline(const FName& VertexShader, const FName& PixelShader, const FRasterDesc& Desc);
        FPipelineH CreateComputePipeline(const FName& ComputeShader);
    }
}
