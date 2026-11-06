#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/CloudRay.slang"

// Builds every half-res cloud texel from this frame's one march per 2x2 block and last frame's clouds, reprojected through the camera's motion and the layer's wind.

struct FPushConstants
{
    FRWTexture2D Scatter;        // this frame's, written whole
    FRWTexture2D CloudDepth;     // this frame's, already holding the marched texel of every 2x2 block
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

    FTexture2D   Fresh;          // this frame's march, one texel per 2x2 block
    uint         _Pad0;
    float2       WindDelta;      // how far the base shape's wind offset moved since the history was marched
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Beyond this relative change in the scene depth under a texel, its history was marched against something else.
static const float kHistoryDepthTolerance = 0.05;

// A marched texel's share against its history, which averages the turning march jitter over its last few visits.
static const float kFreshWeight = 0.2;

// Reprojected history softens with every bilinear resample, so moving content leans harder on the fresh march.
static const float kMovingFreshWeight = 0.5;
static const float kFullMotionPixels  = 4.0;

bool HistoryMatches(float CurrentRaw, float HistoryRaw)
{
    if (CurrentRaw <= 0.0 || HistoryRaw <= 0.0)
    {
        return CurrentRaw <= 0.0 && HistoryRaw <= 0.0;
    }
    const float Current = LinearizeSceneDepth(CurrentRaw);
    return abs(LinearizeSceneDepth(HistoryRaw) - Current) <= Current * kHistoryDepthTolerance;
}

// Bilinear over only the history texels that saw the same scene depth, so cloud hidden behind a silhouette last frame is never revealed as stale.
bool SampleMatchingHistory(float2 PrevUV, float CurrentRaw, int2 Size, out float4 History)
{
    const float2 Pixel = PrevUV * float2(Size) - 0.5;
    const int2   Base  = int2(floor(Pixel));
    const float2 F     = Pixel - float2(Base);

    float4 Sum    = float4(0.0, 0.0, 0.0, 0.0);
    float  Weight = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const int2   Corner = int2(i & 1, i >> 1);
        const int2   Tap    = clamp(Base + Corner, int2(0, 0), Size - 1);
        const float2 Axis   = lerp(1.0 - F, F, float2(Corner));
        const float  W      = Axis.x * Axis.y;
        if (W > 0.0 && HistoryMatches(CurrentRaw, Pass().HistoryDepth.Load(Tap).r))
        {
            Sum    += Pass().History.Load(Tap) * W;
            Weight += W;
        }
    }
    History = Sum / max(Weight, 1e-4);
    return Weight > 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const int2 Size  = int2(Pass().ScreenW, Pass().ScreenH);
    const int2 P     = int2(DTid.xy);
    const int2 Block = P >> 1;
    if (P.x >= Size.x || P.y >= Size.y)
    {
        return;
    }
    const bool bFresh = all(P == Block * 2 + int2(Pass().OffsetX, Pass().OffsetY));

    const FCloudRay Ray = Cloud_SetupRay(Pass().Depth, P, uint2(Size), Pass().LayerBottom, Pass().LayerTop, Pass().MaxDistance);
    if (!bFresh)
    {
        Pass().CloudDepth[P] = float4(Ray.RawDepth, 0.0, 0.0, 0.0);
    }

    // Scene in front of the shell hides the clouds outright, which also keeps moving geometry from dragging stale cloud along.
    if (Ray.End <= Ray.Start)
    {
        Pass().Scatter[P] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // The fresh marches of the surrounding blocks bound what the history may still claim is here.
    const int2 BlockCount = (Size + 1) >> 1;
    const float4 Marched  = Pass().Fresh.Load(Block);
    float4 BoxMin = Marched;
    float4 BoxMax = Marched;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float4 Neighbor = Pass().Fresh.Load(clamp(Block + int2(x, y), int2(0, 0), BlockCount - 1));
            BoxMin = min(BoxMin, Neighbor);
            BoxMax = max(BoxMax, Neighbor);
        }
    }

    // Halfway through the first crossing of the shell stands in for where the cloud mass sits along the ray.
    const float  Distance  = Ray.Start + min(Ray.End - Ray.Start, Ray.OuterR - Ray.InnerR) * 0.5;
    const float3 World     = GetCameraPosition() + Ray.Dir * Distance;
    const float3 PrevWorld = World + float3(Pass().WindDelta.x, 0.0, Pass().WindDelta.y);
    const float4 PrevClip  = mul(GetPrevViewProjection(), float4(PrevWorld, 1.0));
    const float2 PrevUV    = PrevClip.xy / PrevClip.w * 0.5 + 0.5;
    const bool   bOnScreen = PrevClip.w > 0.0 && all(PrevUV >= 0.0) && all(PrevUV <= 1.0);

    float4 History;
    if (!bOnScreen || !SampleMatchingHistory(PrevUV, Ray.RawDepth, Size, History))
    {
        Pass().Scatter[P] = Marched;
        return;
    }
    History = clamp(History, BoxMin, BoxMax);

    if (!bFresh)
    {
        Pass().Scatter[P] = History;
        return;
    }

    const float2 UV          = (float2(P) + 0.5) / float2(Size);
    const float  MotionPx    = length((PrevUV - UV) * float2(Size));
    const float  FreshWeight = lerp(kFreshWeight, kMovingFreshWeight, saturate(MotionPx / kFullMotionPixels));
    Pass().Scatter[P] = lerp(History, Marched, FreshWeight);
}
