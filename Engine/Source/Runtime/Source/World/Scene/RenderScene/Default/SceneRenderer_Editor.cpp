#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
#if USING(WITH_EDITOR)
    // Entity ids belong to the geometry, not any material, so they never enter the GBuffer.
    void FDefaultSceneRenderer::PickerResolvePass(RHI::FCmdListH CL)
    {
        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            return;
        }

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("VisBufferPicker.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Picker Resolve", tracy::Color::SlateBlue);
        SCENE_GPU_SCOPE(CL, "Picker Resolve");

        const FSceneImage& PickerImg = GetNamedImage(ENamedImage::Picker);
        const FSceneImage& VisRT     = GetNamedImage(ENamedImage::VisBuffer);
        const FUIntVector2 Extent    = GetNamedImage(ENamedImage::HDR).GetExtent();

        RHI::FRenderAttachment Color;
        Color.Texture        = PickerImg.Texture;
        Color.LoadOp         = RHI::ELoadOp::Undefined;   // every pixel is written, background included
        Color.StoreOp        = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ PickerImg.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FVisBufferPickerPC
        {
            uint32 VisBufferIndex;
            uint32 DrawListCount;
        } PC = {};
        static_assert(sizeof(FVisBufferPickerPC) == 8, "FVisBufferPickerPC must match VisBufferPicker.slang FVisBufferPickerArgs.");
        PC.VisBufferIndex = (uint32)VisRT.GetResourceID();
        PC.DrawListCount  = DrawListCapacity;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
#endif

#if USING(WITH_EDITOR)
    void FDefaultSceneRenderer::SelectionOutlinePass(RHI::FCmdListH CL)
    {
        const TVector<uint32>& SelectionBits = RenderFrame->Extracts.SelectionBits;
        if (SelectionBits.empty() || !CurrentView->Output.IsValid())
        {
            return;
        }
        
        const FSceneImage& PickerImg = GetNamedImage(ENamedImage::Picker);
        const int32 PickerSlot = PickerImg.IsValid() ? PickerImg.GetResourceID() : -1;
        if (PickerSlot < 0)
        {
            return;
        }

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader  = FShaderLibrary::Get("SelectionOutline.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Selection Outline", tracy::Color::Orange);
        SCENE_GPU_SCOPE(CL, "Selection Outline");

        const FSceneImage& Output = CurrentView->Output;

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, AlphaBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        // Mirrors FSelectionOutlineArgs field for field, IN ORDER; OutlineColor leads for alignment.
        struct FSelectionOutlinePC
        {
            FVector4    OutlineColor;
            RHI::TGPUSpan<uint32> SelectionBits;
            uint32      PickerIndex;
            uint32      EntityIndexMask;
            float       Thickness;
            uint32      _Pad0;
        } PC = {};
        static_assert(sizeof(FSelectionOutlinePC) == 48, "FSelectionOutlinePC must match SelectionOutline.slang.");

        PC.SelectionBits     = RHI::CopyTransientArray(SelectionBits.data(), SelectionBits.size());
        PC.PickerIndex       = (uint32)PickerSlot;
        // From the handle traits rather than a literal, so a change there cannot mask off real index bits.
        PC.EntityIndexMask   = ECS::FEntity::IndexMask;
        PC.Thickness         = 2.0f;
        PC.OutlineColor      = FVector4(1.0f, 0.42f, 0.05f, 1.0f);

        if (!PC.SelectionBits.IsEmpty())
        {
            RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
#endif

    ECS::FEntity FDefaultSceneRenderer::GetEntityAtPixel(uint32 X, uint32 Y) const
    {
    #if USING(WITH_EDITOR)
        int32 BestSlotIdx = -1;
        uint64 BestFrame = 0;
        for (uint32 i = 0; i < PickerReadbackRingSize; ++i)
        {
            const FPickerReadbackSlot& Slot = PickerReadbackRing[i];
            if (!Slot.bPending || Slot.Readback.Gpu == 0)
            {
                continue;
            }
            if (PickerReadbackFrame - Slot.SubmittedFrame <= RHI::kFramesInFlight)
            {
                continue;
            }
            if (X < Slot.OriginX || X >= Slot.OriginX + Slot.Width ||
                Y < Slot.OriginY || Y >= Slot.OriginY + Slot.Height)
            {
                continue;
            }
            if (BestSlotIdx == -1 || Slot.SubmittedFrame > BestFrame)
            {
                BestSlotIdx = static_cast<int32>(i);
                BestFrame = Slot.SubmittedFrame;
            }
        }

        if (BestSlotIdx == -1)
        {
            return ECS::NullEntity;
        }

        const FPickerReadbackSlot& Slot = PickerReadbackRing[BestSlotIdx];
        const uint32 LocalX = X - Slot.OriginX;
        const uint32 LocalY = Y - Slot.OriginY;

        // CPURead allocations are persistently mapped; the readback is tightly packed (RowLength = Width).
        const uint32* Pixels = Slot.Readback.CpuAs<const uint32>();
        if (Pixels == nullptr)
        {
            return ECS::NullEntity;
        }

        const uint32 PixelValue = Pixels[LocalY * Slot.Width + LocalX];

        if (PixelValue == 0)
        {
            return ECS::NullEntity;
        }

        return static_cast<ECS::FEntity>(PixelValue);
    #else
        (void)X;
        (void)Y;
        return ECS::NullEntity;
    #endif
    }

    #if USING(WITH_EDITOR)
    void FDefaultSceneRenderer::SetPickerCursor(uint32 X, uint32 Y, bool bOverViewport)
    {
        const uint64 Packed = (bOverViewport ? 1ull : 0ull)
                            | ((uint64(X) & 0x1FFFFF) << 1)
                            | ((uint64(Y) & 0x1FFFFF) << 22);
        PickerCursorPacked.store(Packed, std::memory_order_relaxed);
    }

    void FDefaultSceneRenderer::IssuePickerReadback(RHI::FCmdListH CL)
    {
        const uint64 Packed = PickerCursorPacked.load(std::memory_order_relaxed);
        const bool bOverViewport = (Packed & 1ull) != 0;
        if (!bOverViewport)
        {
            // The cursor is not over the viewport, so no pick can happen and the copy is skipped.
            return;
        }

        const FSceneImage& PickerImage = GetNamedImage(ENamedImage::Picker);
        if (!PickerImage.IsValid())
        {
            return;
        }

        const uint32 ImgW = PickerImage.GetSizeX();
        const uint32 ImgH = PickerImage.GetSizeY();
        if (ImgW == 0 || ImgH == 0)
        {
            return;
        }

        const uint32 CursorX = Math::Min((uint32)((Packed >> 1) & 0x1FFFFF), ImgW - 1);
        const uint32 CursorY = Math::Min((uint32)((Packed >> 22) & 0x1FFFFF), ImgH - 1);

        const uint32 RegionW = Math::Min(PickerRegionExtent, ImgW);
        const uint32 RegionH = Math::Min(PickerRegionExtent, ImgH);
        const uint32 OriginX = Math::Min(CursorX - Math::Min(CursorX, RegionW / 2), ImgW - RegionW);
        const uint32 OriginY = Math::Min(CursorY - Math::Min(CursorY, RegionH / 2), ImgH - RegionH);

        FPickerReadbackSlot& Slot = PickerReadbackRing[PickerReadbackWriteIndex];

        if (Slot.Readback.Gpu == 0 || Slot.Width != RegionW || Slot.Height != RegionH)
        {
            if (Slot.Readback.Gpu != 0)
            {
                DeferFree(Slot.Readback);
            }
            Slot.Readback = RHI::Malloc((uint64)RegionW * RegionH * sizeof(uint32), RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            RHI::SetDebugName(Slot.Readback.Gpu, "Readback.Picker");
            Slot.Width = RegionW;
            Slot.Height = RegionH;
        }

        RHI::FTextureSlice SrcSlice;
        SrcSlice.Offset = FUIntVector3(OriginX, OriginY, 0);
        SrcSlice.Extent = FUIntVector3(RegionW, RegionH, 1);

        // Picker writes -> transfer read, then host read of the packed region.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite,
            RHI::EStageFlags::Transfer,
            RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
        RHI::CmdCopyTextureToMemory(CL, PickerImage.Texture, SrcSlice, Slot.Readback, RegionW);
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Host,
            RHI::EAccessFlags::HostRead);

        Slot.OriginX = OriginX;
        Slot.OriginY = OriginY;
        Slot.SubmittedFrame = PickerReadbackFrame;
        Slot.bPending = true;

        ++PickerReadbackFrame;
        PickerReadbackWriteIndex = (PickerReadbackWriteIndex + 1) % PickerReadbackRingSize;
    }
    #endif
}
