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

        const uint32 SnapshotRequest = SurfaceSnapshotRequest.load(std::memory_order_acquire);
        bSurfaceSnapshotThisFrame    = bSurfaceSnapshotWanted.load(std::memory_order_acquire) && SnapshotRequest != SurfaceSnapshotServed;
        if (bSurfaceSnapshotThisFrame)
        {
            SurfaceSnapshotServed = SnapshotRequest;
        }

        if (!bOutline && !bHover && !bSurfaceSnapshotThisFrame)
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

        const FSceneImage& PickerImg  = GetNamedImage(ENamedImage::Picker);
        const FSceneImage& FoliageImg = GetNamedImage(ENamedImage::PickerFoliage);
        const FSceneImage& VisRT      = GetNamedImage(ENamedImage::VisBuffer);
        const FUIntVector2 Extent    = GetNamedImage(ENamedImage::HDR).GetExtent();

        // With no mesh draws the VisBuffer was never written, but the outline and the later picker writers still read this image.
        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            RHI::FRenderAttachment Cleared[2];
            Cleared[0].Texture = PickerImg.Texture;
            Cleared[0].LoadOp  = RHI::ELoadOp::Clear;
            Cleared[0].StoreOp = RHI::EStoreOp::Store;
            Cleared[1]         = Cleared[0];
            Cleared[1].Texture = FoliageImg.Texture;

            RHI::FRenderPassDesc ClearPass;
            ClearPass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(Cleared, 2);
            ClearPass.RenderArea       = Extent;
            RHI::CmdBeginRenderPass(CL, ClearPass);
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);
            return;
        }

        RHI::FRenderAttachment Color[2];
        Color[0].Texture     = PickerImg.Texture;
        Color[0].LoadOp      = RHI::ELoadOp::Undefined;   // every pixel is written, background included
        Color[0].StoreOp     = RHI::EStoreOp::Store;
        Color[1]             = Color[0];
        Color[1].Texture     = FoliageImg.Texture;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(Color, 2);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);
        if (!bOutline && !bSurfaceSnapshotThisFrame && Extent.x > 0 && Extent.y > 0)
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
        Key.ColorTargets.push_back({ FoliageImg.Desc.Format, {} });
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
    void FDefaultSceneRenderer::EditorGridPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const FEditorGridRequest& Grid = Frame.Extracts.Grid;
        if (!Grid.bEnabled || !CurrentView->Output.IsValid())
        {
            return;
        }

        const FSceneImage& Depth = GetNamedImage(ENamedImage::DepthAttachment);
        if (!Depth.IsValid())
        {
            return;
        }

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader  = FShaderLibrary::Get("EditorGrid.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Editor Grid", tracy::Color::Gray);

        // The output is past the temporal resolve, so the grid is placed with the camera's unjittered projection.
        const FSceneGlobalData& Globals = Frame.SceneGlobalData;
        FMatrix4 Projection = Globals.CameraData.Projection;
        Projection[2][0] -= Globals.TemporalJitterU * 2.0f;
        Projection[2][1] -= Globals.TemporalJitterV * 2.0f;
        const FMatrix4 ViewProjection = Projection * Globals.CameraData.View;

        struct FEditorGridPC
        {
            FMatrix4 ViewProjection;
            FMatrix4 InverseViewProjection;
            FVector4 MinorColor;
            FVector4 MajorColor;
            FVector4 AxisXColor;
            FVector4 AxisZColor;
            float    Spacing;
            float    FadeDistance;
            float    LineWidth;
            float    AxisWidth;
            uint32   DepthIndex;
            uint32   _Pad0;
            uint32   _Pad1;
            uint32   _Pad2;
        } PC = {};
        static_assert(sizeof(FEditorGridPC) == 224, "FEditorGridPC must match EditorGrid.slang.");

        PC.ViewProjection        = ViewProjection;
        PC.InverseViewProjection = Math::Inverse(ViewProjection);
        PC.MinorColor            = Grid.MinorColor;
        PC.MajorColor            = Grid.MajorColor;
        PC.AxisXColor            = FVector4(0.86f, 0.30f, 0.33f, 0.95f);
        PC.AxisZColor            = FVector4(0.30f, 0.52f, 0.92f, 0.95f);
        PC.Spacing               = Grid.Spacing;
        PC.FadeDistance          = Grid.FadeDistance;
        PC.LineWidth             = Grid.LineWidth;
        PC.AxisWidth             = Grid.AxisWidth;
        PC.DepthIndex            = (uint32)Depth.GetResourceID();

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

        RHI::FBlendDesc PremultipliedBlend;
        PremultipliedBlend.bBlendEnable   = true;
        PremultipliedBlend.SrcColorFactor = RHI::EFactor::One;
        PremultipliedBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        PremultipliedBlend.SrcAlphaFactor = RHI::EFactor::One;
        PremultipliedBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, PremultipliedBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

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

    #if USING(WITH_EDITOR)
    const FDefaultSceneRenderer::FPickerReadbackSlot* FDefaultSceneRenderer::FindPickerReadbackSlot(FUIntVector2 Texel) const
    {
        const FPickerReadbackSlot* Best = nullptr;
        for (const FPickerReadbackSlot& Slot : PickerReadbackRing)
        {
            if (!Slot.bPending || Slot.Readback.Gpu == 0)
            {
                continue;
            }
            if (PickerReadbackFrame - Slot.SubmittedFrame <= RHI::kFramesInFlight)
            {
                continue;
            }
            if (Texel.x < Slot.OriginX || Texel.x >= Slot.OriginX + Slot.Width ||
                Texel.y < Slot.OriginY || Texel.y >= Slot.OriginY + Slot.Height)
            {
                continue;
            }
            if (Best == nullptr || Slot.SubmittedFrame > Best->SubmittedFrame)
            {
                Best = &Slot;
            }
        }
        return Best;
    }
    #endif

    ECS::FEntity FDefaultSceneRenderer::GetEntityAtPixel(uint32 DisplayX, uint32 DisplayY) const
    {
    #if USING(WITH_EDITOR)
        const FUIntVector2 Texel = DisplayToPickerTexel(DisplayX, DisplayY);
        const FPickerReadbackSlot* Slot = FindPickerReadbackSlot(Texel);

        // CPURead allocations are persistently mapped; the readback is tightly packed (RowLength = Width).
        const uint32* Pixels = Slot != nullptr ? Slot->Readback.CpuAs<const uint32>() : nullptr;
        if (Pixels == nullptr)
        {
            return ECS::NullEntity;
        }

        const uint32 PixelValue = Pixels[(Texel.y - Slot->OriginY) * Slot->Width + (Texel.x - Slot->OriginX)];
        return PixelValue == 0 ? ECS::NullEntity : static_cast<ECS::FEntity>(PixelValue);
    #else
        (void)DisplayX;
        (void)DisplayY;
        return ECS::NullEntity;
    #endif
    }

    bool FDefaultSceneRenderer::GetSurfaceAtPixel(uint32 DisplayX, uint32 DisplayY, TSpan<const ECS::FEntity> Ignored, FVector3& OutPosition, FVector3& OutNormal) const
    {
    #if USING(WITH_EDITOR)
        const FUIntVector2 Texel = DisplayToPickerTexel(DisplayX, DisplayY);
        const FPickerReadbackSlot* Slot = FindPickerReadbackSlot(Texel);

        // The snapshot predates whatever is being placed, so it sees under it, but only while the camera matches the live window.
        if (IsSurfaceSnapshotReady() && (Slot == nullptr || Slot->InverseView == SurfaceSnapshot.InverseView)
            && Texel.x < SurfaceSnapshot.Width && Texel.y < SurfaceSnapshot.Height)
        {
            Slot = &SurfaceSnapshot;
        }
        if (Slot == nullptr || Slot->ImageWidth == 0 || Slot->ImageHeight == 0)
        {
            return false;
        }

        const float*  Depths   = Slot->DepthReadback.CpuAs<const float>();
        const uint32* Entities = Slot->Readback.CpuAs<const uint32>();
        const uint8*  Foliage  = Slot->FoliageReadback.CpuAs<const uint8>();
        if (Depths == nullptr || Entities == nullptr || Foliage == nullptr)
        {
            return false;
        }

        const int32 Width  = (int32)Slot->Width;
        const int32 Height = (int32)Slot->Height;

        auto Unproject = [&](int32 LocalX, int32 LocalY, float Depth) -> FVector3
        {
            const float U = ((float)(Slot->OriginX + LocalX) + 0.5f) / (float)Slot->ImageWidth;
            const float V = ((float)(Slot->OriginY + LocalY) + 0.5f) / (float)Slot->ImageHeight;
            const FVector4 World = Slot->InverseViewProjection * FVector4(U * 2.0f - 1.0f, V * 2.0f - 1.0f, Depth, 1.0f);
            return FVector3(World) / World.w;
        };

        // Reverse Z clears to zero, so sky has no depth, and foliage and the ignored entities are read through as if absent.
        auto IsSurface = [&](int32 LocalX, int32 LocalY) -> bool
        {
            if (LocalX < 0 || LocalY < 0 || LocalX >= Width || LocalY >= Height)
            {
                return false;
            }
            const int32 Index = LocalY * Width + LocalX;
            return Depths[Index] > 0.0f && Foliage[Index] == 0
                && !Algo::Contains(Ignored, static_cast<ECS::FEntity>(Entities[Index]));
        };

        auto SurfaceAt = [&](int32 LocalX, int32 LocalY) -> FVector3
        {
            return Unproject(LocalX, LocalY, Depths[LocalY * Width + LocalX]);
        };

        // Each axis takes the neighbor nearer in depth, so a pixel on a silhouette does not borrow the surface behind it.
        auto NormalAt = [&](int32 LocalX, int32 LocalY, const FVector3& Position) -> FVector3
        {
            auto Tangent = [&](int32 StepX, int32 StepY, FVector3& Out) -> bool
            {
                const bool bAhead  = IsSurface(LocalX + StepX, LocalY + StepY);
                const bool bBehind = IsSurface(LocalX - StepX, LocalY - StepY);
                const FVector3 Ahead  = bAhead  ? SurfaceAt(LocalX + StepX, LocalY + StepY) : Position;
                const FVector3 Behind = bBehind ? SurfaceAt(LocalX - StepX, LocalY - StepY) : Position;
                if (bAhead && (!bBehind || Math::Length(Ahead - Position) <= Math::Length(Position - Behind)))
                {
                    Out = Ahead - Position;
                    return true;
                }
                if (bBehind)
                {
                    Out = Position - Behind;
                    return true;
                }
                return false;
            };

            const FVector3 ToCamera = Math::Normalize(Slot->CameraLocation - Position);
            FVector3 AlongX, AlongY;
            if (!Tangent(1, 0, AlongX) || !Tangent(0, 1, AlongY))
            {
                return ToCamera;
            }
            const FVector3 Normal = Math::Normalize(Math::Cross(AlongX, AlongY));
            return Math::Dot(Normal, ToCamera) < 0.0f ? -Normal : Normal;
        };

        const int32 CursorX = (int32)(Texel.x - Slot->OriginX);
        const int32 CursorY = (int32)(Texel.y - Slot->OriginY);
        if (IsSurface(CursorX, CursorY))
        {
            OutPosition = SurfaceAt(CursorX, CursorY);
            OutNormal   = NormalAt(CursorX, CursorY, OutPosition);
            return true;
        }

        // The cursor is over something that does not count, so the nearest surface that does is carried under it as a plane.
        auto TryPlaneFrom = [&](int32 LocalX, int32 LocalY) -> bool
        {
            if (!IsSurface(LocalX, LocalY))
            {
                return false;
            }
            const FVector3 PlanePoint  = SurfaceAt(LocalX, LocalY);
            const FVector3 PlaneNormal = NormalAt(LocalX, LocalY, PlanePoint);

            // Two depths along the cursor's pixel give its ray for a perspective and an orthographic camera alike.
            const FVector3 RayStart = Unproject(CursorX, CursorY, 1.0f);
            const FVector3 RayDir   = Math::Normalize(Unproject(CursorX, CursorY, 0.5f) - RayStart);
            const float    Facing   = Math::Dot(RayDir, PlaneNormal);
            const float    Along    = Math::Abs(Facing) > 1e-4f ? Math::Dot(PlanePoint - RayStart, PlaneNormal) / Facing : -1.0f;

            OutPosition = Along > 0.0f ? RayStart + RayDir * Along : PlanePoint;
            OutNormal   = PlaneNormal;
            return true;
        };

        // Bounded, since the snapshot spans the whole image and a cursor over open sky would otherwise search all of it.
        constexpr int32 MaxSearchRadius = 32;
        for (int32 Radius = 1; Radius <= MaxSearchRadius; ++Radius)
        {
            for (int32 Step = -Radius; Step <= Radius; ++Step)
            {
                if (TryPlaneFrom(CursorX + Step, CursorY - Radius) || TryPlaneFrom(CursorX + Step, CursorY + Radius)
                    || TryPlaneFrom(CursorX - Radius, CursorY + Step) || TryPlaneFrom(CursorX + Radius, CursorY + Step))
                {
                    return true;
                }
            }
        }
        return false;
    #else
        (void)DisplayX;
        (void)DisplayY;
        (void)Ignored;
        (void)OutPosition;
        (void)OutNormal;
        return false;
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

    void FDefaultSceneRenderer::RequestSurfaceSnapshot()
    {
        bSurfaceSnapshotWanted.store(true, std::memory_order_release);
        SurfaceSnapshotRequest.fetch_add(1, std::memory_order_acq_rel);
    }

    void FDefaultSceneRenderer::ReleaseSurfaceSnapshot()
    {
        bSurfaceSnapshotWanted.store(false, std::memory_order_release);
    }

    bool FDefaultSceneRenderer::IsSurfaceSnapshotReady() const
    {
        return bSurfaceSnapshotWanted.load(std::memory_order_acquire)
            && SurfaceSnapshot.bPending
            && SurfaceSnapshotServed == SurfaceSnapshotRequest.load(std::memory_order_acquire)
            && PickerReadbackFrame - SurfaceSnapshot.SubmittedFrame > RHI::kFramesInFlight;
    }

    void FDefaultSceneRenderer::ReleasePickerSlot(FPickerReadbackSlot& Slot)
    {
        for (RHI::FGPUAllocation* Allocation : { &Slot.Readback, &Slot.DepthReadback, &Slot.FoliageReadback })
        {
            if (Allocation->Gpu != 0)
            {
                RHI::Retire(*Allocation);
            }
        }
        Slot = FPickerReadbackSlot{};
    }

    void FDefaultSceneRenderer::CopyPickerRegion(RHI::FCmdListH CL, FPickerReadbackSlot& Slot, uint32 OriginX, uint32 OriginY, uint32 Width, uint32 Height)
    {
        if (Slot.Readback.Gpu == 0 || Slot.Width != Width || Slot.Height != Height)
        {
            ReleasePickerSlot(Slot);
            Slot.Readback = RHI::Malloc((uint64)Width * Height * sizeof(uint32), RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            RHI::SetDebugName(Slot.Readback.Gpu, "Readback.Picker");
            Slot.DepthReadback = RHI::Malloc((uint64)Width * Height * sizeof(float), RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            RHI::SetDebugName(Slot.DepthReadback.Gpu, "Readback.PickerDepth");
            Slot.FoliageReadback = RHI::Malloc((uint64)Width * Height, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            RHI::SetDebugName(Slot.FoliageReadback.Gpu, "Readback.PickerFoliage");
            Slot.Width = Width;
            Slot.Height = Height;
        }

        RHI::FTextureSlice SrcSlice;
        SrcSlice.Offset = FUIntVector3(OriginX, OriginY, 0);
        SrcSlice.Extent = FUIntVector3(Width, Height, 1);

        // Picker and depth writes -> transfer read, then host read of the packed region.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader | RHI::EStageFlags::FragmentTests,
            RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite | RHI::EAccessFlags::DepthStencilWrite,
            RHI::EStageFlags::Transfer,
            RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);

        const FSceneImage& PickerImage = GetNamedImage(ENamedImage::Picker);
        RHI::CmdCopyTextureToMemory(CL, PickerImage.Texture, SrcSlice, Slot.Readback, Width);
        RHI::CmdCopyTextureToMemory(CL, GetNamedImage(ENamedImage::PickerFoliage).Texture, SrcSlice, Slot.FoliageReadback, Width);

        // The depth image shares the picker's render resolution, so the same window lines up texel for texel.
        const FSceneImage& DepthImage = GetNamedImage(ENamedImage::DepthAttachment);
        if (DepthImage.IsValid() && DepthImage.GetSizeX() == PickerImage.GetSizeX() && DepthImage.GetSizeY() == PickerImage.GetSizeY())
        {
            RHI::CmdCopyTextureToMemory(CL, DepthImage.Texture, SrcSlice, Slot.DepthReadback, Width);
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Host,
            RHI::EAccessFlags::HostRead);

        const FCameraData& Camera  = RenderFrame->SceneGlobalData.CameraData;
        Slot.InverseViewProjection = Camera.InverseView * Camera.InverseProjection;
        Slot.InverseView           = Camera.InverseView;
        Slot.CameraLocation        = FVector3(Camera.Location);
        Slot.ImageWidth            = PickerImage.GetSizeX();
        Slot.ImageHeight           = PickerImage.GetSizeY();
        Slot.OriginX               = OriginX;
        Slot.OriginY               = OriginY;
        Slot.SubmittedFrame        = PickerReadbackFrame;
        Slot.bPending              = true;
    }

    void FDefaultSceneRenderer::IssuePickerReadback(RHI::FCmdListH CL)
    {
        // Freed here rather than on release, since the game thread cannot know whether a copy into it is still in flight.
        if (!bSurfaceSnapshotWanted.load(std::memory_order_acquire) && SurfaceSnapshot.Readback.Gpu != 0)
        {
            ReleasePickerSlot(SurfaceSnapshot);
        }

        const FSceneImage& PickerImage = GetNamedImage(ENamedImage::Picker);
        if (!PickerImage.IsValid() || PickerImage.GetSizeX() == 0 || PickerImage.GetSizeY() == 0)
        {
            return;
        }

        if (bSurfaceSnapshotThisFrame)
        {
            bSurfaceSnapshotThisFrame = false;
            CopyPickerRegion(CL, SurfaceSnapshot, 0, 0, PickerImage.GetSizeX(), PickerImage.GetSizeY());
        }

        const uint64 Packed = PickerCursorPacked.load(std::memory_order_relaxed);
        if ((Packed & 1ull) == 0)
        {
            // The cursor is not over the viewport, so no pick can happen and the copy is skipped.
            return;
        }

        const FPickerRegion Region = PickerCursorRegion(Packed, PickerImage.GetSizeX(), PickerImage.GetSizeY(), PickerRegionExtent);
        CopyPickerRegion(CL, PickerReadbackRing[PickerReadbackWriteIndex], Region.X, Region.Y, Region.Width, Region.Height);

        ++PickerReadbackFrame;
        PickerReadbackWriteIndex = (PickerReadbackWriteIndex + 1) % PickerReadbackRingSize;
    }
    #endif
}
