#include "Platform/GenericPlatform.h"
#include "Containers/String.h"
#include "Containers/Name.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/LayoutRegistry.h"

// Hand-written native -> C# bindings for the entire RHI (Runtime/Renderer/RHI.h). Each export mirrors a
// C++ RHI:: free function 1:1 and matches the signature the [NativeCall] generator emits on the C# side
// (LuminaSharp.Rendering.RHI / RHICore): handles + GPUPtr cross as their 8-byte values, blittable descriptor
// structs by value, enums as int32, C++ bool as uint8, spans as (ptr, count), strings as (ptr, len). The
// C# layouts are byte-for-byte mirrors, so span/struct pointers are reinterpret_cast straight through.

using namespace Lumina;

// Wire structs used in the extern "C" signatures below live at file scope (external linkage), not in an
// anonymous namespace, so they are well-formed as C-linkage parameter/return types.

// Scalar mirror of FRasterDesc (no ColorTargets span); matches the C# FRasterDesc wire struct (8 bytes).
struct FRasterWire
{
    uint8 Topology;
    uint8 AlphaToCoverage;
    uint8 Wireframe;
    uint8 SampleCount;
    uint8 DepthFormat;
    uint8 StencilFormat;
    uint8 Reserved0;
    uint8 Reserved1;
};

struct FMemoryTotalsWire
{
    uint64 TotalBudget;
    uint64 TotalUsage;
    uint64 TotalAllocated;
    uint64 TotalBlockBytes;
    uint32 TotalAllocations;
    uint32 TotalBlocks;
    uint32 HeapCount;
};

struct FMemoryHeapWire
{
    uint32 HeapIndex;
    uint8  DeviceLocal;
    uint8  HostVisible;
    uint8  ReBAR;
    uint64 BudgetBytes;
    uint64 UsageBytes;
    uint64 AllocatedBytes;
    uint64 BlockBytes;
    uint32 BlockCount;
    uint32 AllocationCount;
};

namespace
{
    // Two-pass string return: (null, 0) sizes; (buffer, capacity) fills. Returns the full byte length.
    int32 RHICopyOut(const FString& Value, char* Buffer, int32 Capacity)
    {
        const int32 Len = (int32)Value.size();
        if (Buffer != nullptr && Capacity > 0)
        {
            const int32 N = Len < Capacity ? Len : Capacity;
            for (int32 i = 0; i < N; ++i)
            {
                Buffer[i] = Value[(size_t)i];
            }
        }
        return Len;
    }

    FName RHIName(const char* P, int32 Len)
    {
        char Buf[256];
        const int32 N = (Len < 255) ? Len : 255;
        for (int32 i = 0; i < N; ++i) { Buf[i] = P[i]; }
        Buf[N] = 0;
        return FName(Buf);
    }

    RHI::FRasterDesc BuildRaster(const FRasterWire& Wire, const RHI::FColorTarget* Targets, int32 NumTargets)
    {
        RHI::FRasterDesc Desc;
        Desc.Topology         = (RHI::ETopology)Wire.Topology;
        Desc.bAlphaToCoverage = Wire.AlphaToCoverage != 0;
        Desc.bWireframe       = Wire.Wireframe != 0;
        Desc.SampleCount      = Wire.SampleCount;
        Desc.DepthFormat      = (EFormat)Wire.DepthFormat;
        Desc.StencilFormat    = (EFormat)Wire.StencilFormat;
        Desc.ColorTargets     = TSpan<const RHI::FColorTarget>(Targets, (size_t)(NumTargets > 0 ? NumTargets : 0));
        return Desc;
    }
}

//================================================================================================
// Device
//
// The device lifetime (CreateDevice/FreeDevice), the per-frame loop (TickFrame), the window
// swapchain/present, and the device-wide stall (WaitDeviceIdle) are all owned by the engine and
// deliberately NOT exposed to scripts. The managed RHI surface can only query the device and
// synchronize its own submitted work with timeline semaphores.
//================================================================================================

LUMINA_DOTNET_EXPORT(void, RHI_WaitSemaphore)(RHI::FSemaphoreH Semaphore, uint64 Value)
{
    RHI::WaitSemaphore(Semaphore, Value);
}

//================================================================================================
// Memory
//================================================================================================

LUMINA_DOTNET_EXPORT(RHI::GPUPtr, RHI_Malloc)(uint64 Size, uint64 Alignment, int32 Type)
{
    const RHI::GPUPtr Gpu = RHI::Malloc(Size, Alignment, (RHI::EMemoryType)Type);
    RHI::SetDebugName(Gpu, "Script.Buffer");
    return Gpu;
}

LUMINA_DOTNET_EXPORT(void*, RHI_ToHost)(RHI::GPUPtr Gpu)   { return RHI::ToHost(Gpu); }
LUMINA_DOTNET_EXPORT(void, RHI_Free)(RHI::GPUPtr Gpu)      { RHI::Free(Gpu); }

LUMINA_DOTNET_EXPORT(void, RHI_FreeSemaphore)(RHI::FSemaphoreH H)       { RHI::FreeH(H); }
LUMINA_DOTNET_EXPORT(void, RHI_FreePipeline)(RHI::FPipelineH H)         { RHI::FreeH(H); }
LUMINA_DOTNET_EXPORT(void, RHI_FreeTexture)(RHI::FTextureH H)           { RHI::FreeH(H); }
LUMINA_DOTNET_EXPORT(void, RHI_FreeTextureHeap)(RHI::FTextureHeapH H)   { RHI::FreeH(H); }
LUMINA_DOTNET_EXPORT(void, RHI_FreeDepthStencil)(RHI::FDepthStencilH H) { RHI::FreeH(H); }

//================================================================================================
// Resources
//================================================================================================

LUMINA_DOTNET_EXPORT(RHI::FDepthStencilH, RHI_CreateDepthStencil)(RHI::FDepthStencilDesc Desc)
{
    return RHI::CreateDepthStencil(Desc);
}

LUMINA_DOTNET_EXPORT(RHI::FSemaphoreH, RHI_CreateSemaphore)(uint64 Value)
{
    return RHI::CreateTimelineSemaphore(Value);
}

LUMINA_DOTNET_EXPORT(RHI::FTextureH, RHI_CreateTexture)(RHI::FTextureDesc Desc, RHI::GPUPtr Location)
{
    return RHI::CreateTexture(Desc, Location);
}

LUMINA_DOTNET_EXPORT(RHI::FTextureHeapH, RHI_CreateTextureHeap)(uint32 TextureCount, uint32 RWTextureCount, uint32 SamplerCount)
{
    return RHI::CreateTextureHeap(TextureCount, RWTextureCount, SamplerCount);
}

LUMINA_DOTNET_EXPORT(RHI::FTextureDesc, RHI_GetTextureDesc)(RHI::FTextureH Texture)
{
    return RHI::GetTextureDesc(Texture);
}

LUMINA_DOTNET_EXPORT(RHI::FPipelineH, RHI_CreateGraphicsPipeline)(
    const char* VtxCode, int32 VtxLen, const char* VtxEntry, int32 VtxEntryLen,
    const char* FragCode, int32 FragLen, const char* FragEntry, int32 FragEntryLen,
    FRasterWire Raster, const RHI::FColorTarget* Targets, int32 NumTargets,
    const RHI::FSpecializationConstant* Constants, int32 NumConstants)
{
    RHI::FShaderSource Vertex{ TSpan<const std::byte>(reinterpret_cast<const std::byte*>(VtxCode), (size_t)VtxLen), FStringView(VtxEntry, (size_t)VtxEntryLen) };
    RHI::FShaderSource Fragment{ TSpan<const std::byte>(reinterpret_cast<const std::byte*>(FragCode), (size_t)FragLen), FStringView(FragEntry, (size_t)FragEntryLen) };
    RHI::FRasterDesc Desc = BuildRaster(Raster, Targets, NumTargets);
    return RHI::CreateGraphicsPipeline(Vertex, Fragment, Desc,
        TSpan<const RHI::FSpecializationConstant>(Constants, (size_t)(NumConstants > 0 ? NumConstants : 0)));
}

LUMINA_DOTNET_EXPORT(RHI::FPipelineH, RHI_CreateComputePipeline)(
    const char* Code, int32 Len, const char* Entry, int32 EntryLen,
    const RHI::FSpecializationConstant* Constants, int32 NumConstants)
{
    RHI::FShaderSource Compute{ TSpan<const std::byte>(reinterpret_cast<const std::byte*>(Code), (size_t)Len), FStringView(Entry, (size_t)EntryLen) };
    return RHI::CreateComputePipeline(Compute,
        TSpan<const RHI::FSpecializationConstant>(Constants, (size_t)(NumConstants > 0 ? NumConstants : 0)));
}

//================================================================================================
// Texture heap
//================================================================================================

LUMINA_DOTNET_EXPORT(uint32, RHI_HeapWriteTexture)(RHI::FTextureHeapH Heap, RHI::FTextureH Texture)
{
    return RHI::HeapWriteTexture(Heap, Texture);
}

LUMINA_DOTNET_EXPORT(uint32, RHI_HeapWriteRWTexture)(RHI::FTextureHeapH Heap, RHI::FTextureH Texture, uint32 Mip)
{
    return RHI::HeapWriteRWTexture(Heap, Texture, Mip);
}

LUMINA_DOTNET_EXPORT(uint32, RHI_HeapWriteSampler)(RHI::FTextureHeapH Heap, RHI::FSamplerDesc Desc)
{
    return RHI::HeapWriteSampler(Heap, Desc);
}

LUMINA_DOTNET_EXPORT(void, RHI_HeapFreeTexture)(RHI::FTextureHeapH Heap, uint32 Slot)   { RHI::HeapFreeTexture(Heap, Slot); }
LUMINA_DOTNET_EXPORT(void, RHI_HeapFreeRWTexture)(RHI::FTextureHeapH Heap, uint32 Slot) { RHI::HeapFreeRWTexture(Heap, Slot); }
LUMINA_DOTNET_EXPORT(void, RHI_HeapFreeSampler)(RHI::FTextureHeapH Heap, uint32 Slot)   { RHI::HeapFreeSampler(Heap, Slot); }

LUMINA_DOTNET_EXPORT(int32, RHI_HeapTextureCount)(RHI::FTextureHeapH Heap)
{
    TVector<RHI::FHeapTextureInfo> Textures;
    RHI::GetTextureHeapTextures(Heap, Textures);
    return (int32)Textures.size();
}

LUMINA_DOTNET_EXPORT(RHI::FHeapTextureInfo, RHI_HeapTextureAt)(RHI::FTextureHeapH Heap, int32 Index)
{
    TVector<RHI::FHeapTextureInfo> Textures;
    RHI::GetTextureHeapTextures(Heap, Textures);
    if (Index >= 0 && Index < (int32)Textures.size())
    {
        return Textures[(size_t)Index];
    }
    return RHI::FHeapTextureInfo{};
}

//================================================================================================
// Command lists / submission
//================================================================================================

LUMINA_DOTNET_EXPORT(RHI::FCmdListH, RHI_OpenCommandList)(int32 Type)
{
    return RHI::OpenCommandList((RHI::EQueueType)Type);
}

LUMINA_DOTNET_EXPORT(void, RHI_ResetCommandList)(RHI::FCmdListH CommandList)
{
    RHI::ResetCommandList(CommandList);
}

LUMINA_DOTNET_EXPORT(void, RHI_Submit)(RHI::FCmdListH CommandList, int32 Type)
{
    RHI::Submit(CommandList, (RHI::EQueueType)Type);
}

LUMINA_DOTNET_EXPORT(void, RHI_SubmitLists)(int32 Queue,
    const RHI::FCmdListH* CommandLists, int32 NumLists,
    const RHI::FSemaphoreInfo* Waits, int32 NumWaits,
    const RHI::FSemaphoreInfo* Signals, int32 NumSignals)
{
    RHI::Submit((RHI::EQueueType)Queue,
        TSpan<const RHI::FCmdListH>(CommandLists, (size_t)(NumLists > 0 ? NumLists : 0)),
        TSpan<const RHI::FSemaphoreInfo>(Waits, (size_t)(NumWaits > 0 ? NumWaits : 0)),
        TSpan<const RHI::FSemaphoreInfo>(Signals, (size_t)(NumSignals > 0 ? NumSignals : 0)));
}

//================================================================================================
// Device / memory introspection
//================================================================================================

LUMINA_DOTNET_EXPORT(int32, RHI_GetDeviceName)(char* Buffer, int32 Capacity)
{
    return RHICopyOut(RHI::GetDeviceInfo().Name, Buffer, Capacity);
}

LUMINA_DOTNET_EXPORT(int32, RHI_GetDeviceAPIName)(char* Buffer, int32 Capacity)
{
    return RHICopyOut(RHI::GetDeviceInfo().APIName, Buffer, Capacity);
}

LUMINA_DOTNET_EXPORT(uint8, RHI_GetDeviceIsDiscrete)()
{
    return RHI::GetDeviceInfo().bDiscrete ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(FMemoryTotalsWire, RHI_GetMemoryTotals)()
{
    RHI::FGPUMemoryStats Stats;
    RHI::GetGPUMemoryStats(Stats);
    FMemoryTotalsWire Out{};
    Out.TotalBudget      = Stats.TotalBudget;
    Out.TotalUsage       = Stats.TotalUsage;
    Out.TotalAllocated   = Stats.TotalAllocated;
    Out.TotalBlockBytes  = Stats.TotalBlockBytes;
    Out.TotalAllocations = Stats.TotalAllocations;
    Out.TotalBlocks      = Stats.TotalBlocks;
    Out.HeapCount        = (uint32)Stats.Heaps.size();
    return Out;
}

LUMINA_DOTNET_EXPORT(FMemoryHeapWire, RHI_GetMemoryHeap)(int32 HeapIndex)
{
    RHI::FGPUMemoryStats Stats;
    RHI::GetGPUMemoryStats(Stats);
    FMemoryHeapWire Out{};
    if (HeapIndex >= 0 && HeapIndex < (int32)Stats.Heaps.size())
    {
        const RHI::FGPUMemoryHeapStats& H = Stats.Heaps[(size_t)HeapIndex];
        Out.HeapIndex       = H.HeapIndex;
        Out.DeviceLocal     = H.bDeviceLocal ? 1 : 0;
        Out.HostVisible     = H.bHostVisible ? 1 : 0;
        Out.ReBAR           = H.bReBAR ? 1 : 0;
        Out.BudgetBytes     = H.BudgetBytes;
        Out.UsageBytes      = H.UsageBytes;
        Out.AllocatedBytes  = H.AllocatedBytes;
        Out.BlockBytes      = H.BlockBytes;
        Out.BlockCount      = H.BlockCount;
        Out.AllocationCount = H.AllocationCount;
    }
    return Out;
}

//================================================================================================
// Core (RHI::Core)
//================================================================================================

LUMINA_DOTNET_EXPORT(RHI::FTextureHeapH, RHI_CoreGetGlobalHeap)()
{
    return RHI::Core::GetGlobalHeap();
}

LUMINA_DOTNET_EXPORT(RHI::FTransientAlloc, RHI_CoreAllocTransient)(uint64 Size, uint64 Alignment)
{
    return RHI::Core::AllocTransient(Size, Alignment);
}

LUMINA_DOTNET_EXPORT(void, RHI_CoreDeferredFree)(RHI::GPUPtr Memory)
{
    RHI::Core::Retire(Memory);
}

LUMINA_DOTNET_EXPORT(RHI::FPipelineH, RHI_CoreCreateGraphicsPipeline)(
    const char* Vtx, int32 VtxLen, const char* Pix, int32 PixLen,
    FRasterWire Raster, const RHI::FColorTarget* Targets, int32 NumTargets)
{
    RHI::FRasterDesc Desc = BuildRaster(Raster, Targets, NumTargets);
    return RHI::Core::CreateGraphicsPipeline(RHIName(Vtx, VtxLen), RHIName(Pix, PixLen), Desc);
}

LUMINA_DOTNET_EXPORT(RHI::FPipelineH, RHI_CoreCreateComputePipeline)(const char* Compute, int32 Len)
{
    return RHI::Core::CreateComputePipeline(RHIName(Compute, Len));
}

//================================================================================================
// Commands (hot path; SuppressGCTransition on the C# side)
//================================================================================================

LUMINA_DOTNET_EXPORT(void, RHI_CmdMemcpy)(RHI::FCmdListH CL, RHI::GPUPtr Dest, RHI::GPUPtr Source, uint64 Size)
{
    RHI::CmdMemcpy(CL, Dest, Source, (size_t)Size);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdMemset)(RHI::FCmdListH CL, RHI::GPUPtr Dest, uint64 Size, uint32 Value)
{
    RHI::CmdMemset(CL, Dest, Size, Value);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdMemzero)(RHI::FCmdListH CL, RHI::GPUPtr Dest, uint64 Size)
{
    RHI::CmdMemzero(CL, Dest, Size);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdWriteMemory)(RHI::FCmdListH CL, RHI::GPUPtr Dest, const void* Data, int32 Size)
{
    RHI::CmdWriteMemory(CL, Dest, Data, (uint64)(Size > 0 ? Size : 0));
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdCopyTexture)(RHI::FCmdListH CL, RHI::FTextureH Source, RHI::FTextureSlice SourceSlice, RHI::FTextureH Dest, RHI::FTextureSlice DestSlice)
{
    RHI::CmdCopyTexture(CL, Source, SourceSlice, Dest, DestSlice);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdCopyMemoryToTexture)(RHI::FCmdListH CL, RHI::GPUPtr Source, uint32 RowLength, RHI::FTextureH Dest, RHI::FTextureSlice Slice)
{
    RHI::CmdCopyMemoryToTexture(CL, Source, RowLength, Dest, Slice);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdCopyTextureToMemory)(RHI::FCmdListH CL, RHI::FTextureH Source, RHI::FTextureSlice Slice, RHI::GPUPtr Dest, uint32 RowLength)
{
    RHI::CmdCopyTextureToMemory(CL, Source, Slice, Dest, RowLength);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdBlitTexture)(RHI::FCmdListH CL, RHI::FTextureH Source, RHI::FTextureSlice SourceSlice, RHI::FTextureH Dest, RHI::FTextureSlice DestSlice, int32 Filter)
{
    RHI::CmdBlitTexture(CL, Source, SourceSlice, Dest, DestSlice, (RHI::EFilter)Filter);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdResolveTexture)(RHI::FCmdListH CL, RHI::FTextureH Source, RHI::FTextureH Dest)
{
    RHI::CmdResolveTexture(CL, Source, Dest);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdClearTexture)(RHI::FCmdListH CL, RHI::FTextureH Texture, float R, float G, float B, float A)
{
    const float Value[4] = { R, G, B, A };
    RHI::CmdClearTexture(CL, Texture, Value);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdClearTextureUInt)(RHI::FCmdListH CL, RHI::FTextureH Texture, uint32 R, uint32 G, uint32 B, uint32 A)
{
    const uint32 Value[4] = { R, G, B, A };
    RHI::CmdClearTextureUInt(CL, Texture, Value);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdBarrier)(RHI::FCmdListH CL, int32 Before, int32 After)
{
    RHI::CmdBarrier(CL, (RHI::EStageFlags)Before, (RHI::EStageFlags)After);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdBeginRenderPass)(RHI::FCmdListH CL,
    const RHI::FRenderAttachment* ColorAttachments, int32 NumColor,
    RHI::FRenderAttachment Depth, RHI::FRenderAttachment Stencil, FUIntVector2 RenderArea)
{
    RHI::FRenderPassDesc Desc;
    Desc.ColorAttachments = TSpan<const RHI::FRenderAttachment>(ColorAttachments, (size_t)(NumColor > 0 ? NumColor : 0));
    Desc.DepthAttachment   = Depth;
    Desc.StencilAttachment = Stencil;
    Desc.RenderArea        = RenderArea;
    RHI::CmdBeginRenderPass(CL, Desc);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdEndRenderPass)(RHI::FCmdListH CL)                              { RHI::CmdEndRenderPass(CL); }
LUMINA_DOTNET_EXPORT(void, RHI_CmdSetTextureHeap)(RHI::FCmdListH CL, RHI::FTextureHeapH Heap)    { RHI::CmdSetTextureHeap(CL, Heap); }
LUMINA_DOTNET_EXPORT(void, RHI_CmdSetDepthStencilState)(RHI::FCmdListH CL, RHI::FDepthStencilH D){ RHI::CmdSetDepthStencilState(CL, D); }
LUMINA_DOTNET_EXPORT(void, RHI_CmdSetFrontFace)(RHI::FCmdListH CL, int32 Front)                  { RHI::CmdSetFrontFace(CL, (RHI::EFrontFace)Front); }
LUMINA_DOTNET_EXPORT(void, RHI_CmdSetCullMode)(RHI::FCmdListH CL, int32 Mode)                    { RHI::CmdSetCullMode(CL, (RHI::ECullMode)Mode); }
LUMINA_DOTNET_EXPORT(void, RHI_CmdSetLineWidth)(RHI::FCmdListH CL, float Width)                  { RHI::CmdSetLineWidth(CL, Width); }
LUMINA_DOTNET_EXPORT(void, RHI_CmdSetPipeline)(RHI::FCmdListH CL, RHI::FPipelineH Pipeline)      { RHI::CmdSetPipeline(CL, Pipeline); }
LUMINA_DOTNET_EXPORT(void, RHI_CmdSetScissor)(RHI::FCmdListH CL, RHI::FRect Rect)                { RHI::CmdSetScissor(CL, Rect); }
LUMINA_DOTNET_EXPORT(void, RHI_CmdSetViewport)(RHI::FCmdListH CL, RHI::FRect Rect)               { RHI::CmdSetViewport(CL, Rect); }

LUMINA_DOTNET_EXPORT(void, RHI_CmdSetIndexBuffer)(RHI::FCmdListH CL, RHI::GPUPtr IndexBuffer, uint32 Offset, int32 IndexType)
{
    RHI::CmdSetIndexBuffer(CL, IndexBuffer, Offset, (RHI::EIndexType)IndexType);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdDispatch)(RHI::FCmdListH CL, RHI::GPUPtr DrawArgs, uint32 GroupX, uint32 GroupY, uint32 GroupZ)
{
    RHI::CmdDispatch(CL, DrawArgs, GroupX, GroupY, GroupZ);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdDispatchIndirect)(RHI::FCmdListH CL, RHI::GPUPtr DrawArgs, uint32 Offset)
{
    RHI::CmdDispatchIndirect(CL, DrawArgs, Offset);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdDispatchIndirect2)(RHI::FCmdListH CL, RHI::GPUPtr Args, RHI::GPUPtr IndirectBuffer, uint32 Offset)
{
    RHI::CmdDispatchIndirect(CL, Args, IndirectBuffer, Offset);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdDraw)(RHI::FCmdListH CL, RHI::GPUPtr DrawArgs, uint32 VertexCount, uint32 InstanceCount, uint32 FirstVertex, uint32 FirstInstance)
{
    RHI::CmdDraw(CL, DrawArgs, VertexCount, InstanceCount, FirstVertex, FirstInstance);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdDrawIndexed)(RHI::FCmdListH CL, RHI::GPUPtr IndexBuffer, uint32 IndexOffset, RHI::GPUPtr DrawArgs,
    uint32 IndexCount, uint32 InstanceCount, uint32 FirstIndex, int32 VertexOffset, uint32 FirstInstance, int32 IndexType)
{
    RHI::CmdDrawIndexed(CL, IndexBuffer, IndexOffset, DrawArgs, IndexCount, InstanceCount, FirstIndex, VertexOffset, FirstInstance, (RHI::EIndexType)IndexType);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdDrawIndirect)(RHI::FCmdListH CL, RHI::GPUPtr DrawArgs, uint32 Offset, uint32 DrawCount, uint32 Stride)
{
    RHI::CmdDrawIndirect(CL, DrawArgs, Offset, DrawCount, Stride);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdDrawIndirect2)(RHI::FCmdListH CL, RHI::GPUPtr Args, RHI::GPUPtr IndirectBuffer, uint32 Offset, uint32 DrawCount, uint32 Stride)
{
    RHI::CmdDrawIndirect(CL, Args, IndirectBuffer, Offset, DrawCount, Stride);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdDrawIndexedIndirect)(RHI::FCmdListH CL, RHI::GPUPtr DrawArgs, uint32 Offset, uint32 DrawCount, uint32 Stride)
{
    RHI::CmdDrawIndexedIndirect(CL, DrawArgs, Offset, DrawCount, Stride);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdBeginMarker)(RHI::FCmdListH CL, const char* Name, int32 Len)
{
    char Buf[256];
    const int32 N = (Len < 255) ? Len : 255;
    for (int32 i = 0; i < N; ++i) { Buf[i] = Name[i]; }
    Buf[N] = 0;
    RHI::CmdBeginMarker(CL, Buf);
}

LUMINA_DOTNET_EXPORT(void, RHI_CmdEndMarker)(RHI::FCmdListH CL) { RHI::CmdEndMarker(CL); }

//================================================================================================
// Layout guards. The C# blittable mirrors (LuminaSharp.Rendering) are built against these exact native
// sizes; if an RHI struct changes here, the matching C# struct must change in lockstep or the ABI breaks.
//================================================================================================

static_assert(sizeof(FUIntVector2) == 8,  "FUIntVector2 mirror");
static_assert(sizeof(FUIntVector3) == 12, "FUIntVector3 mirror");

static_assert(sizeof(RHI::FRect) == 16,                          "FRect mirror");
static_assert(sizeof(RHI::FTextureDesc) == 32,                   "FTextureDesc mirror");
static_assert(sizeof(RHI::FBlendDesc) == 8,                      "FBlendDesc mirror");
static_assert(sizeof(RHI::FSamplerDesc) == 20,                   "FSamplerDesc mirror");
static_assert(sizeof(RHI::FTextureSlice) == 36,                  "FTextureSlice mirror");
static_assert(sizeof(RHI::FStencil) == 5,                        "FStencil mirror");
static_assert(sizeof(RHI::FDepthStencilDesc) == 28,              "FDepthStencilDesc mirror");
static_assert(sizeof(RHI::FColorTarget) == 9,                    "FColorTarget mirror");
static_assert(sizeof(RHI::FSpecializationConstant) == 24,        "FSpecializationConstant mirror");
static_assert(sizeof(RHI::FSemaphoreInfo) == 24,                 "FSemaphoreInfo mirror");
static_assert(sizeof(RHI::FDrawIndirectArguments) == 16,         "FDrawIndirectArguments mirror");
static_assert(sizeof(RHI::FDrawIndexedIndirectArguments) == 20,  "FDrawIndexedIndirectArguments mirror");
static_assert(sizeof(RHI::FDispatchIndirectArguments) == 12,     "FDispatchIndirectArguments mirror");
static_assert(sizeof(RHI::FRenderAttachment) == 40,              "FRenderAttachment mirror");   // 8-aligned (THandle uint64)
static_assert(sizeof(RHI::FHeapTextureInfo) == 36,               "FHeapTextureInfo mirror");
static_assert(sizeof(RHI::FTransientAlloc) == 16,                "FTransientAlloc mirror");
static_assert(sizeof(RHI::FGPUMemoryHeapStats) == 48,            "FGPUMemoryHeapStats mirror");

// Handles + GPUPtr must stay 8 bytes (the C# mirrors are readonly structs of a single ulong).
static_assert(sizeof(RHI::FTextureH) == 8 && sizeof(RHI::GPUPtr) == 8, "RHI handle/GPUPtr width");

// The pipeline raster wire struct must match the padded C# FRasterDesc (8 bytes).
static_assert(sizeof(FRasterWire) == 8, "FRasterDesc wire mirror");

//================================================================================================
// Layout registry: report each RHI mirror's native sizeof to the managed LayoutValidator (keys match the
// C# [NativeLayout("...")] on each LuminaSharp.Rendering type). A drift on either side fails C# bootstrap.
//================================================================================================

LE_REGISTER_LAYOUT("RHI::FPipelineH",                   RHI::FPipelineH);
LE_REGISTER_LAYOUT("RHI::FTextureH",                    RHI::FTextureH);
LE_REGISTER_LAYOUT("RHI::FTextureHeapH",                RHI::FTextureHeapH);
LE_REGISTER_LAYOUT("RHI::FSemaphoreH",                  RHI::FSemaphoreH);
LE_REGISTER_LAYOUT("RHI::FDepthStencilH",               RHI::FDepthStencilH);
LE_REGISTER_LAYOUT("RHI::FCmdListH",                    RHI::FCmdListH);
LE_REGISTER_LAYOUT("RHI::GPUPtr",                       RHI::GPUPtr);

LE_REGISTER_LAYOUT("RHI::FRect",                        RHI::FRect);
LE_REGISTER_LAYOUT("RHI::FTextureDesc",                 RHI::FTextureDesc);
LE_REGISTER_LAYOUT("RHI::FBlendDesc",                   RHI::FBlendDesc);
LE_REGISTER_LAYOUT("RHI::FSamplerDesc",                 RHI::FSamplerDesc);
LE_REGISTER_LAYOUT("RHI::FTextureSlice",                RHI::FTextureSlice);
LE_REGISTER_LAYOUT("RHI::FStencil",                     RHI::FStencil);
LE_REGISTER_LAYOUT("RHI::FDepthStencilDesc",            RHI::FDepthStencilDesc);
LE_REGISTER_LAYOUT("RHI::FColorTarget",                 RHI::FColorTarget);
LE_REGISTER_LAYOUT("RHI::FSpecializationConstant",      RHI::FSpecializationConstant);
LE_REGISTER_LAYOUT("RHI::FSemaphoreInfo",               RHI::FSemaphoreInfo);
LE_REGISTER_LAYOUT("RHI::FDrawIndirectArguments",       RHI::FDrawIndirectArguments);
LE_REGISTER_LAYOUT("RHI::FDrawIndexedIndirectArguments", RHI::FDrawIndexedIndirectArguments);
LE_REGISTER_LAYOUT("RHI::FDispatchIndirectArguments",   RHI::FDispatchIndirectArguments);
LE_REGISTER_LAYOUT("RHI::FRenderAttachment",            RHI::FRenderAttachment);
LE_REGISTER_LAYOUT("RHI::FHeapTextureInfo",             RHI::FHeapTextureInfo);
LE_REGISTER_LAYOUT("RHI::FTransientAlloc",              RHI::FTransientAlloc);
LE_REGISTER_LAYOUT("RHI::FGPUMemoryHeapStats",          RHI::FGPUMemoryHeapStats);

// Wire structs (no public native counterpart): the C# FRasterDesc / FGPUMemoryTotals mirror these.
LE_REGISTER_LAYOUT("RHI::FRasterDesc",                  FRasterWire);
LE_REGISTER_LAYOUT("RHI::FGPUMemoryTotals",             FMemoryTotalsWire);
