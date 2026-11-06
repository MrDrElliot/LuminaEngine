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

    void FDefaultSceneRenderer::ClusterBuildPass(RHI::FCmdListH CL)
    {
        if (!LightsInternal::HasClusterReaders(*RenderFrame))
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cluster Build Pass", tracy::Color::Pink2);
        
        static const FShaderH ComputeShader = FShaderLibrary::Get("ClusterBuild.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        constexpr uint32 ClusterBuildGroupSize = 64;
        constexpr uint32 ClusterDispatchGroups = RenderUtils::GetGroupCount(MaxClusters, ClusterBuildGroupSize);
        RHI::CmdDispatch(CL, MakeArgs(), ClusterDispatchGroups, 1, 1);

        // LightCull consumes the cluster AABBs next.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::LightCullPass(RHI::FCmdListH CL)
    {
        if (!LightsInternal::HasClusterReaders(*RenderFrame))
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Light Cull Pass", tracy::Color::Pink2);

        static const FShaderH ComputeShader = FShaderLibrary::Get("LightCull.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        // One group per cull block of the largest grid a view can have; groups past this view's grid exit at once.
        RHI::CmdDispatch(CL, MakeArgs(), MaxClusterCullBlocks, 1, 1);

        // Cluster light lists feed the lit pixel shaders, and particles light in the vertex stage.
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

        LUMINA_PROFILE_SECTION_COLORED("Light Function Pass", tracy::Color::Gold);

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

    // Background, terrain and undeferred materials were never classified, so they keep the env pass.
}
