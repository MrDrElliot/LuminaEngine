#pragma once

#include "QuadInstance.h"
#include "Game/Game.h"
#include "Containers/Vector.h"
#include "Renderer/RHI.h"
#include "Renderer/RHITexture.h"
#include "Renderer/RHIUtils.h"

namespace Umbral
{
    inline constexpr int32 kBloomLevels    = 5;
    inline constexpr int32 kMaxDrawnAgents = 120000;
    inline constexpr int32 kMaxLights      = 512;

    struct FLightInstance
    {
        FVector2 Center;
        float    Radius;
        float    Energy;
        FVector4 Color;
    };

    static_assert(sizeof(FLightInstance) == 32, "The light shader assumes a packed 32 byte instance.");

    class FRenderer
    {
    public:

        bool Initialize(EFormat InSwapchainFormat);
        void Shutdown();

        void EnsureTargets(const FUIntVector2& Extent);
        void Render(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent, FGame& Game, float RealTime);

    private:

        bool CreatePipelines();
        void ReleaseTargets();

        void Gather(FGame& Game, float RealTime);
        void GatherHud(FGame& Game, float RealTime);

        void DrawLights(RHI::FCmdListH CL);
        void DrawScene(RHI::FCmdListH CL, const FUIntVector2& Extent, float RealTime, const FRunState& Run);
        void DrawBloom(RHI::FCmdListH CL);
        void DrawComposite(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent,
                           float RealTime, const FRunState& Run, const FPlayerState& Player, bool bPaused);

        EFormat SwapchainFormat = EFormat::UNKNOWN;

        RHI::FPipelineH GroundPipeline;
        RHI::FPipelineH LightPipeline;
        RHI::FPipelineH AgentPipeline;
        RHI::FPipelineH QuadAlphaPipeline;
        RHI::FPipelineH QuadAdditivePipeline;
        RHI::FPipelineH DownsamplePipeline;
        RHI::FPipelineH UpsamplePipeline;
        RHI::FPipelineH CompositePipeline;

        RHI::FManagedTexture SceneTarget;
        RHI::FManagedTexture LightTarget;
        RHI::Utils::FMipChain BloomChain;
        FUIntVector2 LightExtent { 0, 0 };
        FUIntVector2 TargetExtent { 0, 0 };

        TVector<FQuadInstance>  AlphaQuads;
        TVector<FQuadInstance>  AdditiveQuads;
        TVector<FQuadInstance>  UiQuads;
        TVector<FQuadInstance>  UiGlowQuads;
        TVector<FLightInstance> Lights;

        RHI::GPUPtr AgentBuffer = 0;
        int32 AgentCount = 0;

        FVector2 CameraMin { 0.0f, 0.0f };
        FVector2 PlayerWorld { 0.0f, 0.0f };
        FVector2 ViewSize  { kViewWidth, kViewHeight };
        float    UnitsToPixels = 1.0f;
        float    BeatPulse = 0.0f;
    };
}
