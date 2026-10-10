#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
#if USING(WITH_EDITOR)
    namespace
    {
        struct FPickerRegion
        {
            uint32 X = 0;
            uint32 Y = 0;
            uint32 Width = 0;
            uint32 Height = 0;
        };

        // The window around the cursor that the readback copies, so the resolve and the copy agree on it.
        FPickerRegion PickerCursorRegion(uint64 Packed, uint32 ImgW, uint32 ImgH, uint32 Extent)
        {
            const uint32 CursorX = Math::Min((uint32)((Packed >> 1) & 0x1FFFFF), ImgW - 1);
            const uint32 CursorY = Math::Min((uint32)((Packed >> 22) & 0x1FFFFF), ImgH - 1);

            FPickerRegion Region;
            Region.Width  = Math::Min(Extent, ImgW);
            Region.Height = Math::Min(Extent, ImgH);
            Region.X      = Math::Min(CursorX - Math::Min(CursorX, Region.Width / 2), ImgW - Region.Width);
            Region.Y      = Math::Min(CursorY - Math::Min(CursorY, Region.Height / 2), ImgH - Region.Height);
            return Region;
        }
    }

    // Entity ids belong to the geometry, not any material, so they never enter the GBuffer.
    void FDefaultSceneRenderer::PickerResolvePass(RHI::FCmdListH CL)
    {
        // Only the selection outline reads the whole image; a hover reads one cursor window, and otherwise nothing does.
        const bool   bOutline = !RenderFrame->Extracts.SelectionBits.empty();
        const uint64 Cursor   = PickerCursorPacked.load(std::memory_order_relaxed);
        const bool   bHover   = (Cursor & 1ull) != 0;
        if (!bOutline && !bHover)
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

        // With no mesh draws the VisBuffer was never written, but the outline and the later picker writers still read this image.
        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            RHI::FRenderAttachment Cleared;
            Cleared.Texture = PickerImg.Texture;
            Cleared.LoadOp  = RHI::ELoadOp::Clear;
            Cleared.StoreOp = RHI::EStoreOp::Store;

            RHI::FRenderPassDesc ClearPass;
            ClearPass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Cleared, 1);
            ClearPass.RenderArea       = Extent;
            RHI::CmdBeginRenderPass(CL, ClearPass);
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);
            return;
        }

        RHI::FRenderAttachment Color;
        Color.Texture        = PickerImg.Texture;
        Color.LoadOp         = RHI::ELoadOp::Undefined;   // every pixel is written, background included
        Color.StoreOp        = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);
        if (!bOutline && Extent.x > 0 && Extent.y > 0)
        {
            const FPickerRegion Region = PickerCursorRegion(Cursor, Extent.x, Extent.y, PickerRegionExtent);
            RHI::CmdSetScissor(CL, RHI::FRect{ (int)Region.X, (int)(Region.X + Region.Width), (int)Region.Y, (int)(Region.Y + Region.Height) });
        }
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
    static TConsoleVar<bool> CVarSelectionOutlineTiles("r.Editor.SelectionOutlineTiles", true,
        "Skip the selection outline's neighborhood test outside the tiles a selected pixel can reach.");

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
        static const FShaderH QuadShader   = FShaderLibrary::Get("SelectionOutlineTileQuad.slang");
        static const FShaderH PixelShader  = FShaderLibrary::Get("SelectionOutline.slang");
        static const FShaderH TileShader   = FShaderLibrary::Get("SelectionOutlineTiles.slang");
        if (!VertexShader || !QuadShader || !PixelShader || !TileShader)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Selection Outline", tracy::Color::Orange);
        SCENE_GPU_SCOPE(CL, "Selection Outline");

        const FSceneImage& Output = CurrentView->Output;

        constexpr float  OutlineThickness = 2.0f;
        constexpr uint32 OutlineTileSize  = 8;
        const FUIntVector2 Extent = Output.GetExtent();
        const uint32 TilesX   = (Extent.x + OutlineTileSize - 1u) / OutlineTileSize;
        const uint32 TilesY   = (Extent.y + OutlineTileSize - 1u) / OutlineTileSize;
        const uint32 NumTiles = TilesX * TilesY;

        const RHI::TGPUSpan<uint32> SelectionSpan = RHI::CopyTransientArray(SelectionBits.data(), SelectionBits.size());

        ReserveBuffer(CL, SelectionTileStampBuffer, (uint64)NumTiles * sizeof(uint32));
        ReserveBuffer(CL, SelectionTileListBuffer, (uint64)NumTiles * sizeof(uint32));
        ReserveBuffer(CL, SelectionOutlineDrawArgsBuffer, sizeof(RHI::FDrawIndirectArguments));
        const bool bTiled = CVarSelectionOutlineTiles.GetValue()
                         && SelectionTileStampBuffer.CapacityOf<uint32>() >= NumTiles
                         && SelectionTileListBuffer.CapacityOf<uint32>() >= NumTiles
                         && SelectionOutlineDrawArgsBuffer.Size >= sizeof(RHI::FDrawIndirectArguments);

        const RHI::FGPURange DrawArgs{ SelectionOutlineDrawArgsBuffer.Gpu, sizeof(RHI::FDrawIndirectArguments) };
        const RHI::TGPUSpan<uint32> TileList{ SelectionTileListBuffer, NumTiles };

        if (bTiled)
        {
            // Zero is what a fresh buffer holds, so no frame may stamp with it.
            if (++SelectionTileStamp == 0u)
            {
                SelectionTileStamp = 1u;
            }

            struct FSelectionTilePC
            {
                RHI::TGPUSpan<uint32> SelectionBits;
                RHI::TGPUSpan<uint32> TileStamps;
                RHI::TGPUSpan<uint32> TileList;
                RHI::TGPUSpan<uint32> DrawArgs;
                uint32 PickerIndex;
                uint32 EntityIndexMask;
                uint32 TileStamp;
                uint32 TilesX;
                uint32 TilesY;
                uint32 TileReach;
                uint32 Pad0;
                uint32 Pad1;
            } TilePC = {};
            static_assert(sizeof(FSelectionTilePC) == 96, "FSelectionTilePC must match SelectionOutlineTiles.slang.");

            TilePC.SelectionBits   = SelectionSpan;
            TilePC.TileStamps      = RHI::TGPUSpan<uint32>{ SelectionTileStampBuffer, NumTiles };
            TilePC.TileList        = TileList;
            TilePC.DrawArgs        = RHI::TGPUSpan<uint32>::FromAddress(DrawArgs.Address, 4u);
            TilePC.PickerIndex     = (uint32)PickerSlot;
            TilePC.EntityIndexMask = ECS::FEntity::IndexMask;
            TilePC.TileStamp       = SelectionTileStamp;
            TilePC.TilesX          = TilesX;
            TilePC.TilesY          = TilesY;
            TilePC.TileReach       = ((uint32)Math::Ceil(OutlineThickness) + OutlineTileSize - 1u) / OutlineTileSize;

            // The previous frame's draw read the list and its arguments, and this frame rewrites both.
            RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::IndirectRead,
                            RHI::EStageFlags::Transfer | RHI::EStageFlags::Compute, RHI::EAccessFlags::TransferWrite | RHI::EAccessFlags::ShaderWrite);
            RHI::CmdMemzero(CL, DrawArgs);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            DispatchCompute(CL, TileShader, TilePC, TilesX, TilesY, 1u);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                            RHI::EStageFlags::AllCommands, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::IndirectRead);
        }

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

        // Tile quads cover only what the selection can reach, and every pixel they cover runs the full-screen test unchanged.
        FGraphicsPipelineKey Key;
        Key.VS = bTiled ? QuadShader : VertexShader;
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
            RHI::TGPUSpan<uint32> TileList;
            uint32      _Pad1;
            uint32      _Pad2;
            uint32      _Pad3;
            uint32      _Pad4;
        } PC = {};
        static_assert(sizeof(FSelectionOutlinePC) == 80, "FSelectionOutlinePC must match SelectionOutline.slang.");

        PC.SelectionBits     = SelectionSpan;
        PC.PickerIndex       = (uint32)PickerSlot;
        // From the handle traits rather than a literal, so a change there cannot mask off real index bits.
        PC.EntityIndexMask   = ECS::FEntity::IndexMask;
        PC.Thickness         = OutlineThickness;
        PC.TileList          = TileList;
        PC.OutlineColor      = FVector4(1.0f, 0.42f, 0.05f, 1.0f);

        if (!PC.SelectionBits.IsEmpty())
        {
            if (bTiled)
            {
                RHI::CmdDrawIndirect(CL, MakeArgs(PC), DrawArgs, 1u, sizeof(RHI::FDrawIndirectArguments));
            }
            else
            {
                RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
#endif

    FUIntVector2 FDefaultSceneRenderer::DisplayToPickerTexel(uint32 X, uint32 Y) const
    {
        if (SceneViews.empty())
        {
            return FUIntVector2(X, Y);
        }
        const FSceneView& Primary = SceneViews[0];
        const uint64 PickerX = (uint64)X * Primary.Size.x / Math::Max(Primary.DisplaySize.x, 1u);
        const uint64 PickerY = (uint64)Y * Primary.Size.y / Math::Max(Primary.DisplaySize.y, 1u);
        return FUIntVector2((uint32)PickerX, (uint32)PickerY);
    }

    ECS::FEntity FDefaultSceneRenderer::GetEntityAtPixel(uint32 DisplayX, uint32 DisplayY) const
    {
    #if USING(WITH_EDITOR)
        const FUIntVector2 Texel = DisplayToPickerTexel(DisplayX, DisplayY);
        const uint32 X = Texel.x;
        const uint32 Y = Texel.y;

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
        (void)DisplayX;
        (void)DisplayY;
        return ECS::NullEntity;
    #endif
    }

    #if USING(WITH_EDITOR)
    void FDefaultSceneRenderer::SetPickerCursor(uint32 DisplayX, uint32 DisplayY, bool bOverViewport)
    {
        const FUIntVector2 Texel = DisplayToPickerTexel(DisplayX, DisplayY);
        const uint32 X = Texel.x;
        const uint32 Y = Texel.y;
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

        const FPickerRegion Region = PickerCursorRegion(Packed, ImgW, ImgH, PickerRegionExtent);
        const uint32 RegionW = Region.Width;
        const uint32 RegionH = Region.Height;
        const uint32 OriginX = Region.X;
        const uint32 OriginY = Region.Y;

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
