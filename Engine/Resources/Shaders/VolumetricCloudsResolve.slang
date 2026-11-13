#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/CloudRay.slang"

// Builds every half-res cloud texel from this frame's one march per 2x2 block, or that texel's own last march reprojected through the camera's motion and the layer's wind.

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

// Beyond this relative change in scene depth, two texels' rays were marched against different surfaces.
static const float kSameDepthTolerance = 0.05;

bool SameSceneDepth(float CurrentRaw, float OtherRaw)
{
    if (CurrentRaw <= 0.0 || OtherRaw <= 0.0)
    {
        return CurrentRaw <= 0.0 && OtherRaw <= 0.0;
    }
    const float Current = LinearizeSceneDepth(CurrentRaw);
    return abs(LinearizeSceneDepth(OtherRaw) - Current) <= Current * kSameDepthTolerance;
}

// Bilinear over only the taps whose ray saw the same scene depth, so cloud never bleeds across a silhouette, from last frame's texels or this frame's block marches.
bool SampleMatchingDepth(float2 Pixel, float CurrentRaw, int2 Size, bool bFromFresh, out float4 Result)
{
    const int2   Base   = int2(floor(Pixel));
    const float2 F      = Pixel - float2(Base);
    const int2   Offset = int2(Pass().OffsetX, Pass().OffsetY);
    const int2   Screen = int2(Pass().ScreenW, Pass().ScreenH);

    float4 Sum    = float4(0.0, 0.0, 0.0, 0.0);
    float  Weight = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const int2   Corner = int2(i & 1, i >> 1);
        const int2   Tap    = clamp(Base + Corner, int2(0, 0), Size - 1);
        const float2 Axis   = lerp(1.0 - F, F, float2(Corner));
        const float  W      = Axis.x * Axis.y;
        const float  TapRaw = bFromFresh ? Pass().CloudDepth[min(Tap * 2 + Offset, Screen - 1)].r : Pass().HistoryDepth.Load(Tap).r;
        if (W > 0.0 && SameSceneDepth(CurrentRaw, TapRaw))
        {
            Sum    += (bFromFresh ? Pass().Fresh.Load(Tap) : Pass().History.Load(Tap)) * W;
            Weight += W;
        }
    }
    Result = Sum / max(Weight, 1e-4);
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

    // A marched texel is taken as is, so nothing here needs more than one frame to look right.
    const float4 Marched = Pass().Fresh.Load(Block);
    if (bFresh)
    {
        Pass().Scatter[P] = Marched;
        return;
    }

    // The fresh marches of the surrounding blocks bound what the history may still claim is here.
    const int2 BlockCount = (Size + 1) >> 1;
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
    if (!bOnScreen || !SampleMatchingDepth(PrevUV * float2(Size) - 0.5, Ray.RawDepth, Size, false, History))
    {
        const float2 BlockPixel = float2(P - int2(Pass().OffsetX, Pass().OffsetY)) * 0.5;
        Pass().Scatter[P] = SampleMatchingDepth(BlockPixel, Ray.RawDepth, BlockCount, true, History) ? History : Marched;
        return;
    }
    Pass().Scatter[P] = clamp(History, BoxMin, BoxMax);
}
