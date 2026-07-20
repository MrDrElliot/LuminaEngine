using System;
using Lumina;
using LuminaSharp;
using LuminaSharp.Rendering;

namespace Game;

// Minimal C# world renderer: clears its render target to a pulsing color each frame. Its presence
// alone makes the engine render every Game (PIE/standalone) world through it.
public sealed class SampleRenderScene : RenderScene
{
    private FTextureH Target;
    private uint HeapSlot = uint.MaxValue;
    private FUIntVector2 Size = new(1280, 720);
    private float Time;

    public override void OnInit()
    {
        CreateTarget();
    }

    public override void OnShutdown()
    {
        ReleaseTarget();
    }

    public override void OnRender(int FrameIndex)
    {
        Time += 1.0f / 60.0f;

        FCmdListH CL = RHI.OpenCommandList();

        Span<FRenderAttachment> Color = stackalloc FRenderAttachment[1];
        Color[0] = new FRenderAttachment
        {
            Texture = Target,
            LoadOp = ELoadOp.Clear,
            StoreOp = EStoreOp.Store,
            ClearR = 0.5f + 0.5f * MathF.Sin(Time),
            ClearG = 0.2f,
            ClearB = 0.5f + 0.5f * MathF.Cos(Time),
            ClearA = 1.0f,
        };
        RHI.CmdBeginRenderPass(CL, Color, Size);
        RHI.CmdEndRenderPass(CL);
        RHI.Barriers.RasterToRead(CL);

        RHI.Submit(CL);
    }

    public override void OnResize(uint Width, uint Height)
    {
        ReleaseTarget();
        Size = new FUIntVector2(Math.Max(Width, 1u), Math.Max(Height, 1u));
        CreateTarget();
    }

    public override FTextureH DisplayTexture => Target;
    public override uint DisplayResourceID => HeapSlot;
    public override FUIntVector2 RenderExtent => Size;

    private void CreateTarget()
    {
        Target = RHI.CreateTexture(FTextureDesc.Texture2D(Size.X, Size.Y, EFormat.RGBA8_UNORM,
            EImageUsageFlags.ColorAttachment | EImageUsageFlags.Sampled | EImageUsageFlags.TransferSrc));
        HeapSlot = RHI.HeapWriteTexture(RHICore.GetGlobalHeap(), Target);
    }

    private void ReleaseTarget()
    {
        if (HeapSlot != uint.MaxValue)
        {
            RHI.HeapFreeTexture(RHICore.GetGlobalHeap(), HeapSlot);
            HeapSlot = uint.MaxValue;
        }
        if (Target.IsValid)
        {
            RHI.FreeH(Target);
            Target = default;
        }
    }
}
