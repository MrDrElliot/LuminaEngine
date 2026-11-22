#pragma once

#include "RHI.h"

// Immediate destruction, reached only from the retire drain once the GPU is done with the object.
namespace Lumina::RHI::Internal
{
    void CreateDevice(const FDeviceDesc& Desc);
    void FreeDevice();

    // The lock that orders submits to one queue, shared by aliased queue types.
    void LockQueue(EQueueType Queue);
    void UnlockQueue(EQueueType Queue);

    // The raw queue submit, called with LockQueue held. Every public path routes through the queue timelines in RHICore.cpp instead.
    void Submit(EQueueType Queue, TSpan<const FCmdListH> CommandLists, TSpan<const FSemaphoreInfo> Waits, TSpan<const FSemaphoreInfo> Signals);

    void DestroyNow(const FGPUAllocation& Allocation);
    void DestroyNow(FTextureH Texture);
    void DestroyNow(FPipelineH Pipeline);
    void DestroyNow(FSemaphoreH Semaphore);
    void DestroyNow(FTextureHeapH Heap);
    void DestroyNow(FSwapchainH Swapchain);
    void DestroyNow(FSurfaceH Surface);
#if defined(LUMINA_WITH_GPU_PROFILING)
    void DestroyNow(FQueryPoolH Pool);
#endif
}
