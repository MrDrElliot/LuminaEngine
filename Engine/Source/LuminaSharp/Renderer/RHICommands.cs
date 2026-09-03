using System;
using Lumina;

namespace LuminaSharp.Rendering;

// The command-recording surface of the RHI. These are the hot path: each just appends to a per-thread command
// list, so they are bound with SuppressGCTransition (no GC-mode switch) for the lowest possible call cost.
public static unsafe partial class RHI
{
    // ---- Memory commands ----

    [NativeCall("LuminaSharp_RHI_CmdMemcpy", SuppressGCTransition = true)]
    public static partial void CmdMemcpy(FCmdListH CL, GPUPtr Dest, GPUPtr Source, ulong Size);

    [NativeCall("LuminaSharp_RHI_CmdMemset", SuppressGCTransition = true)]
    public static partial void CmdMemset(FCmdListH CL, GPUPtr Dest, ulong Size, uint Value);

    [NativeCall("LuminaSharp_RHI_CmdMemzero", SuppressGCTransition = true)]
    public static partial void CmdMemzero(FCmdListH CL, GPUPtr Dest, ulong Size);

    /// <summary>Inline a CPU byte range into the command stream and write it to <paramref name="Dest"/>.</summary>
    [NativeCall("LuminaSharp_RHI_CmdWriteMemory", SuppressGCTransition = true)]
    public static partial void CmdWriteMemory(FCmdListH CL, GPUPtr Dest, ReadOnlySpan<byte> Data);

    // ---- Texture commands ----

    [NativeCall("LuminaSharp_RHI_CmdCopyTexture", SuppressGCTransition = true)]
    public static partial void CmdCopyTexture(FCmdListH CL, FTextureH Source, FTextureSlice SourceSlice, FTextureH Dest, FTextureSlice DestSlice);

    [NativeCall("LuminaSharp_RHI_CmdCopyMemoryToTexture", SuppressGCTransition = true)]
    public static partial void CmdCopyMemoryToTexture(FCmdListH CL, GPUPtr Source, uint RowLength, FTextureH Dest, FTextureSlice Slice);

    public static void CmdCopyMemoryToTexture(FCmdListH CL, GPUPtr Source, uint RowLength, FTextureH Dest)
        => CmdCopyMemoryToTexture(CL, Source, RowLength, Dest, FTextureSlice.Full);

    [NativeCall("LuminaSharp_RHI_CmdCopyTextureToMemory", SuppressGCTransition = true)]
    public static partial void CmdCopyTextureToMemory(FCmdListH CL, FTextureH Source, FTextureSlice Slice, GPUPtr Dest, uint RowLength = 0);

    [NativeCall("LuminaSharp_RHI_CmdBlitTexture", SuppressGCTransition = true)]
    public static partial void CmdBlitTexture(FCmdListH CL, FTextureH Source, FTextureSlice SourceSlice, FTextureH Dest, FTextureSlice DestSlice, EFilter Filter = EFilter.Linear);

    [NativeCall("LuminaSharp_RHI_CmdResolveTexture", SuppressGCTransition = true)]
    public static partial void CmdResolveTexture(FCmdListH CL, FTextureH Source, FTextureH Dest);

    [NativeCall("LuminaSharp_RHI_CmdClearTexture", SuppressGCTransition = true)]
    public static partial void CmdClearTexture(FCmdListH CL, FTextureH Texture, float R, float G, float B, float A);

    [NativeCall("LuminaSharp_RHI_CmdClearTextureUInt", SuppressGCTransition = true)]
    public static partial void CmdClearTextureUInt(FCmdListH CL, FTextureH Texture, uint R, uint G, uint B, uint A);

    // ---- Barriers ----

    [NativeCall("LuminaSharp_RHI_CmdBarrier", SuppressGCTransition = true)]
    public static partial void CmdBarrier(FCmdListH CL, EStageFlags Before, EStageFlags After);

    /// <summary>Canonical pipeline-stage barriers (mirrors C++ <c>RHI::Barriers</c>).</summary>
    public static class Barriers
    {
        public static void ComputeToAll(FCmdListH CL) => CmdBarrier(CL,
            EStageFlags.Compute,
            EStageFlags.Compute | EStageFlags.VertexShader | EStageFlags.PixelShader |
            EStageFlags.IndirectArguments | EStageFlags.FragmentTests | EStageFlags.Transfer);

        public static void RasterToRead(FCmdListH CL) => CmdBarrier(CL,
            EStageFlags.RasterColorOut | EStageFlags.FragmentTests,
            EStageFlags.PixelShader | EStageFlags.VertexShader | EStageFlags.Compute |
            EStageFlags.RasterColorOut | EStageFlags.FragmentTests);

        public static void RasterToRaster(FCmdListH CL) => CmdBarrier(CL,
            EStageFlags.RasterColorOut | EStageFlags.FragmentTests,
            EStageFlags.RasterColorOut | EStageFlags.FragmentTests);

        public static void TransferToAll(FCmdListH CL) => CmdBarrier(CL, EStageFlags.Transfer, EStageFlags.AllCommands);

        public static void AllToTransfer(FCmdListH CL) => CmdBarrier(CL, EStageFlags.AllCommands, EStageFlags.Transfer);
    }

    // ---- Render pass ----

    [NativeCall("LuminaSharp_RHI_CmdBeginRenderPass")]
    public static partial void CmdBeginRenderPass(FCmdListH CL, ReadOnlySpan<FRenderAttachment> ColorAttachments,
        FRenderAttachment Depth, FRenderAttachment Stencil, FUIntVector2 RenderArea);

    public static void CmdBeginRenderPass(FCmdListH CL, ReadOnlySpan<FRenderAttachment> ColorAttachments, FUIntVector2 RenderArea)
        => CmdBeginRenderPass(CL, ColorAttachments, default, default, RenderArea);

    [NativeCall("LuminaSharp_RHI_CmdEndRenderPass", SuppressGCTransition = true)]
    public static partial void CmdEndRenderPass(FCmdListH CL);

    // ---- Pipeline / fixed-function state ----

    [NativeCall("LuminaSharp_RHI_CmdSetTextureHeap", SuppressGCTransition = true)]
    public static partial void CmdSetTextureHeap(FCmdListH CL, FTextureHeapH Heap);

    [NativeCall("LuminaSharp_RHI_CmdSetDepthStencilState", SuppressGCTransition = true)]
    public static partial void CmdSetDepthStencilState(FCmdListH CL, FDepthStencilH DepthStencil);

    [NativeCall("LuminaSharp_RHI_CmdSetFrontFace", SuppressGCTransition = true)]
    public static partial void CmdSetFrontFace(FCmdListH CL, EFrontFace Front);

    [NativeCall("LuminaSharp_RHI_CmdSetCullMode", SuppressGCTransition = true)]
    public static partial void CmdSetCullMode(FCmdListH CL, ECullMode Mode);

    [NativeCall("LuminaSharp_RHI_CmdSetLineWidth", SuppressGCTransition = true)]
    public static partial void CmdSetLineWidth(FCmdListH CL, float Width);

    [NativeCall("LuminaSharp_RHI_CmdSetPipeline", SuppressGCTransition = true)]
    public static partial void CmdSetPipeline(FCmdListH CL, FPipelineH Pipeline);

    [NativeCall("LuminaSharp_RHI_CmdSetScissor", SuppressGCTransition = true)]
    public static partial void CmdSetScissor(FCmdListH CL, FRect Rect);

    [NativeCall("LuminaSharp_RHI_CmdSetViewport", SuppressGCTransition = true)]
    public static partial void CmdSetViewport(FCmdListH CL, FRect Rect);

    [NativeCall("LuminaSharp_RHI_CmdSetIndexBuffer", SuppressGCTransition = true)]
    public static partial void CmdSetIndexBuffer(FCmdListH CL, GPUPtr IndexBuffer, uint Offset = 0, EIndexType IndexType = EIndexType.Uint32);

    // ---- Dispatch / draw ----

    [NativeCall("LuminaSharp_RHI_CmdDispatch", SuppressGCTransition = true)]
    public static partial void CmdDispatch(FCmdListH CL, GPUPtr DrawArgs, uint GroupX, uint GroupY, uint GroupZ);

    [NativeCall("LuminaSharp_RHI_CmdDispatchIndirect", SuppressGCTransition = true)]
    private static partial void CmdDispatchIndirectCore(FCmdListH CL, GPUPtr DrawArgs, uint Offset);
    public static void CmdDispatchIndirect(FCmdListH CL, GPUPtr DrawArgs, uint Offset) => CmdDispatchIndirectCore(CL, DrawArgs, Offset);

    [NativeCall("LuminaSharp_RHI_CmdDispatchIndirect2", SuppressGCTransition = true)]
    private static partial void CmdDispatchIndirectBufferCore(FCmdListH CL, GPUPtr Args, GPUPtr IndirectBuffer, uint Offset);
    public static void CmdDispatchIndirect(FCmdListH CL, GPUPtr Args, GPUPtr IndirectBuffer, uint Offset) => CmdDispatchIndirectBufferCore(CL, Args, IndirectBuffer, Offset);

    [NativeCall("LuminaSharp_RHI_CmdDraw", SuppressGCTransition = true)]
    public static partial void CmdDraw(FCmdListH CL, GPUPtr DrawArgs, uint VertexCount, uint InstanceCount, uint FirstVertex, uint FirstInstance);

    [NativeCall("LuminaSharp_RHI_CmdDrawIndexed", SuppressGCTransition = true)]
    public static partial void CmdDrawIndexed(FCmdListH CL, GPUPtr IndexBuffer, uint IndexOffset, GPUPtr DrawArgs,
        uint IndexCount, uint InstanceCount, uint FirstIndex, int VertexOffset, uint FirstInstance, EIndexType IndexType = EIndexType.Uint32);

    [NativeCall("LuminaSharp_RHI_CmdDrawIndirect", SuppressGCTransition = true)]
    private static partial void CmdDrawIndirectCore(FCmdListH CL, GPUPtr DrawArgs, uint Offset, uint DrawCount, uint Stride);
    public static void CmdDrawIndirect(FCmdListH CL, GPUPtr DrawArgs, uint Offset, uint DrawCount, uint Stride) => CmdDrawIndirectCore(CL, DrawArgs, Offset, DrawCount, Stride);

    [NativeCall("LuminaSharp_RHI_CmdDrawIndirect2", SuppressGCTransition = true)]
    private static partial void CmdDrawIndirectBufferCore(FCmdListH CL, GPUPtr Args, GPUPtr IndirectBuffer, uint Offset, uint DrawCount, uint Stride);
    public static void CmdDrawIndirect(FCmdListH CL, GPUPtr Args, GPUPtr IndirectBuffer, uint Offset, uint DrawCount, uint Stride) => CmdDrawIndirectBufferCore(CL, Args, IndirectBuffer, Offset, DrawCount, Stride);

    [NativeCall("LuminaSharp_RHI_CmdDrawIndexedIndirect", SuppressGCTransition = true)]
    private static partial void CmdDrawIndexedIndirectCore(FCmdListH CL, GPUPtr DrawArgs, uint Offset, uint DrawCount, uint Stride);
    public static void CmdDrawIndexedIndirect(FCmdListH CL, GPUPtr DrawArgs, uint Offset, uint DrawCount, uint Stride) => CmdDrawIndexedIndirectCore(CL, DrawArgs, Offset, DrawCount, Stride);

    [NativeCall("LuminaSharp_RHI_CmdDrawIndexedIndirect2", SuppressGCTransition = true)]
    private static partial void CmdDrawIndexedIndirectBufferCore(FCmdListH CL, GPUPtr Args, GPUPtr IndirectBuffer, uint Offset, uint DrawCount, uint Stride);
    public static void CmdDrawIndexedIndirect(FCmdListH CL, GPUPtr Args, GPUPtr IndirectBuffer, uint Offset, uint DrawCount, uint Stride) => CmdDrawIndexedIndirectBufferCore(CL, Args, IndirectBuffer, Offset, DrawCount, Stride);

    // ---- Debug markers ----

    [NativeCall("LuminaSharp_RHI_CmdBeginMarker")]
    public static partial void CmdBeginMarker(FCmdListH CL, string Name);

    [NativeCall("LuminaSharp_RHI_CmdEndMarker", SuppressGCTransition = true)]
    public static partial void CmdEndMarker(FCmdListH CL);
}
