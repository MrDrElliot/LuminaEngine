#pragma once

#include "QuadInstance.h"
#include "Game/Game.h"
#include "Containers/Vector.h"
#include "Renderer/RHI.h"
#include "Renderer/RHITexture.h"
#include "Renderer/RHIUtils.h"

namespace Breakout
{
    inline constexpr int32 kBloomLevels = 5;

    class FRenderer
    {
    public:

        bool Initialize(EFormat InSwapchainFormat);
        void Shutdown();

        // Recreates the offscreen targets on a resize, so it must run outside an open command list.
        void EnsureTargets(const FUIntVector2& Extent);

        void Render(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent, FGame& Game, float RealTime);

    private:

        bool CreatePipelines();
        void ReleaseTargets();

        void GatherQuads(FGame& Game, float RealTime);
        void GatherHud(FGame& Game, float RealTime);

        void DrawScene(RHI::FCmdListH CL, const FUIntVector2& Extent, float RealTime, const FGameState& State);
        void DrawBloom(RHI::FCmdListH CL);
        void DrawComposite(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent,
                           float RealTime, const FGameState& State, const FCameraShake& Shake, bool bPaused);

        void GatherWarps(FGame& Game);

        FVector4 Warps[4] {};
        float    ShieldGlow = 0.0f;
        float    BeatPulse = 0.0f;

        EFormat SwapchainFormat = EFormat::UNKNOWN;

        RHI::FPipelineH BackgroundPipeline;
        RHI::FPipelineH QuadAlphaPipeline;
        RHI::FPipelineH QuadAdditivePipeline;
        RHI::FPipelineH DownsamplePipeline;
        RHI::FPipelineH UpsamplePipeline;
        RHI::FPipelineH CompositePipeline;

        RHI::FManagedTexture SceneTarget;
        RHI::Utils::FMipChain BloomChain;
        FUIntVector2 TargetExtent { 0, 0 };

        TVector<FQuadInstance> AlphaQuads;
        TVector<FQuadInstance> AdditiveQuads;

        FVector2 FieldOriginPixels { 0.0f, 0.0f };
        FVector2 FieldSizePixels   { 0.0f, 0.0f };
        float    UnitsToPixels     = 1.0f;
    };
}
