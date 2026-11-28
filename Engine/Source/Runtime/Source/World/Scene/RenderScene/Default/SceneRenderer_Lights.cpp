#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace LightsInternal
    {
        // Particles, volumetric fog, water and weather shade from the cluster lists too, not only meshes and terrain.
        bool HasClusterReaders(const FDefaultSceneRenderer::FFrameData& Frame)
        {
            return !Frame.Geometry.DrawCommands.IsEmpty()
                || !Frame.Extracts.TerrainExtracts.IsEmpty()
                || !Frame.Extracts.ParticleExtracts.IsEmpty()
                || (Frame.Volumetrics.bHasFog && Frame.Volumetrics.bVolumetricFog)
                || !Frame.Water.Surfaces.IsEmpty()
                || Frame.Weather.bActive;
        }
    }

    namespace LightsInternal
    {
        struct FLightUploadArgs
        {
            RHI::TGPUSpan<FSceneLightSet::FUpload> Uploads;
        };

        void ComputeToCompute(RHI::FCmdListH CL)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }
    }

    void FDefaultSceneRenderer::UploadRetainedLights(RHI::FCmdListH CL)
    {
        FFrameData::FLighting& Lighting = RenderFrame->Lighting;
        if (!Lighting.bRetainedUploadPending)
        {
            return;
        }
        Lighting.bRetainedUploadPending = false;

        if (bRetainedLightsFresh)
        {
            bRetainedLightsFresh = false;
            RHI::CmdMemset(CL, RetainedLightBuffer, 0u);
            RHI::CmdMemset(CL, RetainedLightBoundsBuffer, 0u);
            RHI::CmdMemset(CL, RetainedLightLayerBuffer, 0u);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }

        const uint32 NumUploads = (uint32)Lighting.RetainedUploads.Num();
        if (NumUploads == 0u)
        {
            return;
        }

        static const FShaderH ComputeShader = FShaderLibrary::Get("LightUpload.slang");
        if (!ComputeShader)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Light Upload");
        LightsInternal::FLightUploadArgs Args;
        Args.Uploads = RHI::TGPUSpan<FSceneLightSet::FUpload>(RHI::CopyTransientArray(Lighting.RetainedUploads.GetData(), NumUploads));
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));
        RHI::CmdDispatch(CL, MakeArgs(Args), RenderUtils::GetGroupCount(NumUploads, LIGHT_CULL_GROUP_SIZE), 1, 1);
        LightsInternal::ComputeToCompute(CL);
    }

    // Cull, bin, clear, scatter and grid build, all sized on the GPU, so no pass waits on the CPU to learn how many lights a view sees.
    void FDefaultSceneRenderer::LightCullPass(RHI::FCmdListH CL)
    {
        if (!LightsInternal::HasClusterReaders(*RenderFrame))
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Light Cull Pass", tracy::Color::Pink2);

        static const FShaderH CullShader    = FShaderLibrary::Get("LightCull.slang");
        static const FShaderH ScanShader    = FShaderLibrary::Get("LightBinScan.slang");
        static const FShaderH ClearShader   = FShaderLibrary::Get("LightMaskClear.slang");
        static const FShaderH ScatterShader = FShaderLibrary::Get("LightScatter.slang");
        static const FShaderH GridShader    = FShaderLibrary::Get("LightGridBuild.slang");
        if (!CullShader || !ScanShader || !ClearShader || !ScatterShader || !GridShader || CurrentView == nullptr)
        {
            return;
        }

        UploadRetainedLights(CL);

        FSceneView& View = *CurrentView;

        // A probe bake culls each face into the same grid the face before it just shaded from.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite);

        if (View.bLightGridFresh)
        {
            View.bLightGridFresh = false;
            RHI::CmdMemset(CL, View.LightGridBuffer, 0u);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }

        const uint32 Capacity = RenderFrame->Lighting.RetainedCapacity;
        {
            SCENE_GPU_SCOPE(CL, "Light Cull");
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullShader));
            RHI::CmdDispatch(CL, MakeArgs(), RenderUtils::GetGroupCount(Capacity, LIGHT_CULL_GROUP_SIZE), 1, 1);
            LightsInternal::ComputeToCompute(CL);
        }
        {
            SCENE_GPU_SCOPE(CL, "Light Bin Scan");
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ScanShader));
            RHI::CmdDispatch(CL, MakeArgs(), 1, 1, 1);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);
        }
        {
            SCENE_GPU_SCOPE(CL, "Light Mask Clear");
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ClearShader));
            RHI::CmdDispatchIndirect(CL, MakeArgs(), View.LightGridBuffer.Sub(LIGHT_GRID_CLEAR_ARGS * sizeof(uint32), sizeof(uint32) * 3u));
            LightsInternal::ComputeToCompute(CL);
        }
        const RHI::FGPURange PerLightArgs = View.LightGridBuffer.Sub(LIGHT_GRID_SCATTER_ARGS * sizeof(uint32), sizeof(uint32) * 3u);
        {
            SCENE_GPU_SCOPE(CL, "Light Scatter");
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ScatterShader));
            RHI::CmdDispatchIndirect(CL, MakeArgs(), PerLightArgs);
            LightsInternal::ComputeToCompute(CL);
        }
        {
            SCENE_GPU_SCOPE(CL, "Light Grid Build");
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(GridShader));
            RHI::CmdDispatchIndirect(CL, MakeArgs(), PerLightArgs);
        }

        // The sorted list and its grid feed the lit pixel shaders, and particles light in the vertex stage.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    namespace
    {
        // Must match FLightFunctionArgs in LightFunctionPixelPass.slang.
        struct FLightFunctionArgs
        {
            uint32   MaterialIndex;
            uint32   LightFlags;
            float    _Pad[2];
            FVector4 Position;
            FVector4 Forward;
            FVector4 Right;
            FVector4 Up;
        };
        static_assert(sizeof(FLightFunctionArgs) == 80, "FLightFunctionArgs must match LightFunctionPixelPass.slang");
    }

    void FDefaultSceneRenderer::LightFunctionPass(RHI::FCmdListH CL)
    {
        const auto& Draws = RenderFrame->Lighting.LightFunctionDraws;
        if (Draws.IsEmpty() || !LightFunctionAtlas)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Light Functions");

        // Cleared to white, so a tile whose pipeline is still compiling leaves its light unmasked.
        RHI::FRenderAttachment Color;
        Color.Texture = LightFunctionAtlas.Texture;
        Color.LoadOp  = RHI::ELoadOp::Clear;
        Color.StoreOp = RHI::EStoreOp::Store;
        Color.Color[0] = Color.Color[1] = Color.Color[2] = Color.Color[3] = 1.0f;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = LightFunctionAtlas.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        for (const FFrameData::FLighting::FLightFunctionDraw& Draw : Draws)
        {
            FGraphicsPipelineKey Key;
            Key.VS = Draw.Shaders.VertexShader;
            Key.PS = Draw.Shaders.PixelShader;
            Key.ColorTargets.Add({ LightFunctionAtlas.Desc.Format, {} });
            const RHI::FPipelineH Pipeline = FindPipeline(Key);
            if (!Pipeline)
            {
                continue;
            }

            const int32 TileX = (int32)((Draw.Slot % LIGHT_FUNCTION_ATLAS_TILES) * LIGHT_FUNCTION_TILE_SIZE);
            const int32 TileY = (int32)((Draw.Slot / LIGHT_FUNCTION_ATLAS_TILES) * LIGHT_FUNCTION_TILE_SIZE);
            const RHI::FRect TileRect{ TileX, TileX + (int32)LIGHT_FUNCTION_TILE_SIZE, TileY, TileY + (int32)LIGHT_FUNCTION_TILE_SIZE };
            RHI::CmdSetViewport(CL, TileRect);
            RHI::CmdSetScissor(CL, TileRect);
            RHI::CmdSetPipeline(CL, Pipeline);

            const FLightFunctionRequest& Request = Draw.Request;
            FLightFunctionArgs Args = {};
            Args.MaterialIndex = Draw.MaterialIndex;
            Args.LightFlags    = (uint32)Request.Type;
            Args.Position      = FVector4(Request.Position, 1.0f);
            Args.Forward       = FVector4(Request.Forward, 0.0f);
            Args.Right         = FVector4(Request.Right, Request.ProjectionScale);
            Args.Up            = FVector4(Request.Up, 0.0f);
            RHI::CmdDraw(CL, MakeArgs(Args), 3, 1, 0, 0);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
}
