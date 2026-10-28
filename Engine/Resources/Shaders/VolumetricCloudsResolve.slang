#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/CloudRay.slang"

// Fills the half-res cloud texels the march skipped this frame from last frame's clouds, reprojected through the camera's motion.

struct FPushConstants
{
    FRWTexture2D Scatter;        // this frame's, holding the marched texel of every 2x2 block
    FRWTexture2D CloudDepth;     // this frame's
    FTexture2D   History;        // last frame's scatter
    FTexture2D   HistoryDepth;   // the scene depth last frame's texels were marched against

    FTexture2D   Depth;
    uint         ScreenW;
    uint         ScreenH;
    uint         OffsetX;

    uint         OffsetY;
    float        LayerBottom;
    float        LayerTop;
    float        MaxDistance;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Beyond this relative change in the scene depth under a texel, its history was marched against something else.
static const float kHistoryDepthTolerance = 0.05;

// A marched texel's share against its history, which averages the turning march jitter over its last few visits.
static const float kFreshWeight = 0.2;

bool HistoryMatches(float CurrentRaw, float HistoryRaw)
{
    if (CurrentRaw <= 0.0 || HistoryRaw <= 0.0)
    {
        return CurrentRaw <= 0.0 && HistoryRaw <= 0.0;
    }
    const float Current = LinearizeSceneDepth(CurrentRaw);
    return abs(LinearizeSceneDepth(HistoryRaw) - Current) <= Current * kHistoryDepthTolerance;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 Size  = uint2(Pass().ScreenW, Pass().ScreenH);
    const int2  P     = int2(DTid.xy);
    const int2  Fresh = (P & ~1) + int2(Pass().OffsetX, Pass().OffsetY);
    if (P.x >= int(Size.x) || P.y >= int(Size.y))
    {
        return;
    }
    const bool bFresh = all(P == Fresh);

    const FCloudRay Ray = Cloud_SetupRay(Pass().Depth, P, Size, Pass().LayerBottom, Pass().LayerTop, Pass().MaxDistance);
    if (!bFresh)
    {
        Pass().CloudDepth[P] = float4(Ray.RawDepth, 0.0, 0.0, 0.0);
    }

    // Scene in front of the shell hides the clouds outright, which also keeps moving geometry from dragging stale cloud along.
    if (Ray.End <= Ray.Start)
    {
        if (!bFresh)
        {
            Pass().Scatter[P] = float4(0.0, 0.0, 0.0, 1.0);
        }
        return;
    }

    // Halfway through the first crossing of the shell stands in for where the cloud mass sits along the ray.
    const float  Distance  = Ray.Start + min(Ray.End - Ray.Start, Ray.OuterR - Ray.InnerR) * 0.5;
    const float3 World     = GetCameraPosition() + Ray.Dir * Distance;
    const float4 PrevClip  = mul(GetPrevViewProjection(), float4(World, 1.0));
    const float2 PrevUV    = PrevClip.xy / PrevClip.w * 0.5 + 0.5;
    const bool   bOnScreen = PrevClip.w > 0.0 && all(PrevUV >= 0.0) && all(PrevUV <= 1.0);

    // The marched neighbor of the same block, for a texel whose history is off screen or saw different geometry.
    const int2   FreshInBounds = min(Fresh, int2(Size) - 1);
    const float4 Marched       = Pass().Scatter[FreshInBounds];
    const bool   bHistory      = bOnScreen && HistoryMatches(Ray.RawDepth, Pass().HistoryDepth.SampleLevel(Samplers::PointClamp, PrevUV, 0.0).r);
    if (!bHistory)
    {
        if (!bFresh)
        {
            Pass().Scatter[P] = Marched;
        }
        return;
    }

    const float4 History = Pass().History.SampleLevel(Samplers::LinearClamp, PrevUV, 0.0);
    Pass().Scatter[P] = bFresh ? lerp(History, Marched, kFreshWeight) : History;
}
