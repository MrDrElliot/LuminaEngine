using System;
using Lumina;

namespace LuminaSharp.Rendering;

/// Direct 1:1 binding to the engine RHI (Lumina::RHI); blittable handles/descriptors cross by value with zero marshalling. Command lists are per-thread, recording is lock-free, creation/submission are synchronized. Device and frame loop are engine-owned.
public static unsafe partial class RHI
{
    // Device. Engine-owned: scripts cannot create/destroy/tick/present/stall it; wait on submitted work via a timeline semaphore.

    [NativeCall("LuminaSharp_RHI_WaitSemaphore")]
    public static partial void WaitSemaphore(FSemaphoreH Semaphore, ulong Value);

    // Memory.

    /// Allocate device memory; the result carries the device address, the CPU mapping and the size.
    [NativeCall("LuminaSharp_RHI_Malloc")]
    public static partial FGPUAllocation Malloc(ulong Size, ulong Alignment = 16, EMemoryType Type = EMemoryType.Default);

    /// Allocate device memory of the given type at default alignment.
    public static FGPUAllocation Malloc(ulong Size, EMemoryType Type) => Malloc(Size, 16, Type);

    [NativeCall("LuminaSharp_RHI_Retire")]
    public static partial void Retire(FGPUAllocation Allocation);

    // Typed thunks have unique names (the generator keys on method name); the overloads forward to them.
    [NativeCall("LuminaSharp_RHI_RetireSemaphore")] private static partial void RetireSemaphore(FSemaphoreH H);
    [NativeCall("LuminaSharp_RHI_RetirePipeline")] private static partial void RetirePipeline(FPipelineH H);
    [NativeCall("LuminaSharp_RHI_RetireTexture")] private static partial void RetireTexture(FTextureH H);
    [NativeCall("LuminaSharp_RHI_RetireTextureHeap")] private static partial void RetireTextureHeap(FTextureHeapH H);

    public static void Retire(FSemaphoreH Semaphore) => RetireSemaphore(Semaphore);
    public static void Retire(FPipelineH Pipeline) => RetirePipeline(Pipeline);
    public static void Retire(FTextureH Texture) => RetireTexture(Texture);
    public static void Retire(FTextureHeapH Heap) => RetireTextureHeap(Heap);

    // Resources.


    [NativeCall("LuminaSharp_RHI_CreateSemaphore")]
    public static partial FSemaphoreH CreateSemaphore(ulong Value);

    /// Create a texture; omit Location (the default) for engine-managed backing memory.
    [NativeCall("LuminaSharp_RHI_CreateTexture")]
    public static partial FTextureH CreateTexture(FTextureDesc Desc, GPUPtr Location = default);

    [NativeCall("LuminaSharp_RHI_CreateTextureHeap")]
    public static partial FTextureHeapH CreateTextureHeap(uint TextureCount, uint RWTextureCount, uint SamplerCount);

    [NativeCall("LuminaSharp_RHI_GetTextureDesc", SuppressGCTransition = true)]
    public static partial FTextureDesc GetTextureDesc(FTextureH Texture);

    /// Create a graphics pipeline from compiled shader bytecode; ColorTargets and Constants may be empty. See RHICore.CreateGraphicsPipeline for named shaders.
    [NativeCall("LuminaSharp_RHI_CreateGraphicsPipeline")]
    public static partial FPipelineH CreateGraphicsPipeline(
        ReadOnlySpan<byte> VertexCode, string VertexEntry,
        ReadOnlySpan<byte> FragmentCode, string FragmentEntry,
        FRasterDesc Raster, ReadOnlySpan<FColorTarget> ColorTargets,
        ReadOnlySpan<FSpecializationConstant> Constants);

    [NativeCall("LuminaSharp_RHI_CreateComputePipeline")]
    public static partial FPipelineH CreateComputePipeline(
        ReadOnlySpan<byte> ComputeCode, string ComputeEntry, ReadOnlySpan<FSpecializationConstant> Constants);

    // Texture heap.

    [NativeCall("LuminaSharp_RHI_HeapWriteTexture")]
    public static partial uint HeapWriteTexture(FTextureHeapH Heap, FTextureH Texture);

    /// Write a storage-image slot; Mip defaults to 0.
    [NativeCall("LuminaSharp_RHI_HeapWriteRWTexture")]
    public static partial uint HeapWriteRWTexture(FTextureHeapH Heap, FTextureH Texture, uint Mip = 0);

    [NativeCall("LuminaSharp_RHI_HeapWriteSampler")]
    public static partial uint HeapWriteSampler(FTextureHeapH Heap, FSamplerDesc Desc);

    [NativeCall("LuminaSharp_RHI_HeapFreeTexture")]
    public static partial void HeapFreeTexture(FTextureHeapH Heap, uint Slot);
    [NativeCall("LuminaSharp_RHI_HeapFreeRWTexture")]
    public static partial void HeapFreeRWTexture(FTextureHeapH Heap, uint Slot);
    [NativeCall("LuminaSharp_RHI_HeapFreeSampler")]
    public static partial void HeapFreeSampler(FTextureHeapH Heap, uint Slot);

    [NativeCall("LuminaSharp_RHI_HeapTextureCount")]
    public static partial int HeapTextureCount(FTextureHeapH Heap);

    [NativeCall("LuminaSharp_RHI_HeapTextureAt")]
    public static partial FHeapTextureInfo HeapTextureAt(FTextureHeapH Heap, int Index);

    /// Debug: every occupied sampled slot in the heap.
    public static FHeapTextureInfo[] GetTextureHeapTextures(FTextureHeapH Heap)
    {
        int Count = HeapTextureCount(Heap);
        if (Count <= 0)
        {
            return Array.Empty<FHeapTextureInfo>();
        }
        FHeapTextureInfo[] Result = new FHeapTextureInfo[Count];
        for (int i = 0; i < Count; i++)
        {
            Result[i] = HeapTextureAt(Heap, i);
        }
        return Result;
    }

    // Command lists / submission.

    /// Open a command list on the given queue; Type defaults to the graphics queue.
    [NativeCall("LuminaSharp_RHI_OpenCommandList")]
    public static partial FCmdListH OpenCommandList(EQueueType Type = EQueueType.Default);

    [NativeCall("LuminaSharp_RHI_ResetCommandList")]
    public static partial void ResetCommandList(FCmdListH CommandList);

    [NativeCall("LuminaSharp_RHI_SubmitLists")]
    private static partial ulong SubmitListsCore(EQueueType Queue, ReadOnlySpan<FCmdListH> CommandLists,
        ReadOnlySpan<FSemaphoreInfo> Waits);

    /// Submit command lists with optional timeline waits and return the queue timeline value they signal.
    /// The frame ring recycles the lists, so never reset one after submitting it.
    public static ulong Submit(EQueueType Queue, ReadOnlySpan<FCmdListH> CommandLists,
        ReadOnlySpan<FSemaphoreInfo> Waits = default)
        => SubmitListsCore(Queue, CommandLists, Waits);

    /// One command list on the given queue; the span form is the same call.
    public static ulong Submit(FCmdListH CommandList, EQueueType Queue = EQueueType.Default)
    {
        ReadOnlySpan<FCmdListH> Lists = stackalloc FCmdListH[1] { CommandList };
        return SubmitListsCore(Queue, Lists, default);
    }

    // Device / memory introspection.

    [NativeCall("LuminaSharp_RHI_GetMemoryTotals")]
    public static partial FGPUMemoryTotals GetMemoryTotals();

    [NativeCall("LuminaSharp_RHI_GetMemoryHeap")]
    public static partial FGPUMemoryHeapStats GetMemoryHeap(int HeapIndex);

    /// Full per-heap GPU memory breakdown.
    public static (FGPUMemoryTotals Totals, FGPUMemoryHeapStats[] Heaps) GetGPUMemoryStats()
    {
        FGPUMemoryTotals Totals = GetMemoryTotals();
        FGPUMemoryHeapStats[] Heaps = new FGPUMemoryHeapStats[Totals.HeapCount];
        for (int i = 0; i < Heaps.Length; i++)
        {
            Heaps[i] = GetMemoryHeap(i);
        }
        return (Totals, Heaps);
    }

    [NativeCall("LuminaSharp_RHI_GetDeviceName")]
    private static partial string GetDeviceName();
    [NativeCall("LuminaSharp_RHI_GetDeviceAPIName")]
    private static partial string GetDeviceAPIName();
    [NativeCall("LuminaSharp_RHI_GetDeviceIsDiscrete")]
    private static partial bool GetDeviceIsDiscrete();

    /// API-neutral GPU device summary.
    public static GPUDeviceInfo GetDeviceInfo() => new(GetDeviceName(), GetDeviceAPIName(), GetDeviceIsDiscrete());

}

/// Runtime support layer for the RHI (RHI::Core): global texture/sampler heap, per-frame transient ring, deferred frees, and named-shader pipeline creation.
public static unsafe partial class RHICore
{
    /// The engine's global texture/sampler heap.
    [NativeCall("LuminaSharp_RHI_CoreGetGlobalHeap")]
    public static partial FTextureHeapH GetGlobalHeap();

    /// Per-frame transient GPU memory (CPU-write, device-addressable), valid until the slot recycles; Alignment defaults to 16 bytes.
    [NativeCall("LuminaSharp_RHI_CoreAllocTransient")]
    public static partial FTransientAlloc AllocTransient(ulong Size, ulong Alignment = 16);

    /// Free GPU memory once every in-flight frame has retired.
    [NativeCall("LuminaSharp_RHI_CoreDeferredFree")]
    public static partial void DeferredFree(FGPUAllocation Memory);

    /// Build a graphics pipeline from named shaders in the engine shader library (compiles/caches).
    [NativeCall("LuminaSharp_RHI_CoreCreateGraphicsPipeline")]
    public static partial FPipelineH CreateGraphicsPipeline(string VertexShader, string PixelShader,
        FRasterDesc Raster, ReadOnlySpan<FColorTarget> ColorTargets);

    /// Build a compute pipeline from a named shader in the engine shader library.
    [NativeCall("LuminaSharp_RHI_CoreCreateComputePipeline")]
    public static partial FPipelineH CreateComputePipeline(string ComputeShader);
}

/// API-neutral GPU device summary (RHI::FGPUDeviceInfo).
public sealed class GPUDeviceInfo
{
    public string Name { get; }       // e.g. "NVIDIA GeForce RTX 4080"
    public string APIName { get; }    // e.g. "Vulkan 1.4.250"
    public bool IsDiscrete { get; }

    internal GPUDeviceInfo(string Name, string APIName, bool IsDiscrete)
    {
        this.Name = Name;
        this.APIName = APIName;
        this.IsDiscrete = IsDiscrete;
    }

    public override string ToString() => $"{Name} ({APIName}{(IsDiscrete ? ", discrete" : "")})";
}
