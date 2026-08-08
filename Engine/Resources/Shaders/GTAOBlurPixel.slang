#pragma once
#include "Includes/SceneGlobals.slang"

// Plane-aware bilateral denoise of the raw GTAO factor, at half res. Each tap is weighted by how
// close its view depth is to the depth PREDICTED by the center's local slope (quad-derivative
// plane fit), so tilted surfaces blur fully -- a flat center-depth tolerance collapses on slanted
// faces and lets the raw slice noise through -- while silhouettes still reject. The separate
// upsample pass (GTAOUpsamplePixel.slang) then reconstructs full res.

struct FragmentInput
{
    float2 UV : TEXCOORD0;
};

struct FData
{
    uint AOIndex;      // bindless SRV of the raw AO texture (half res)
    uint DepthIndex;   // bindless SRV of the full-res scene depth buffer
};

float ViewDepth(uint DepthIndex, float2 UV)
{
    // Half-res UVs land on full-res texel corners; snap to a center so the point
    // sample doesn't flip-flop between texels (reads as scanline noise in the weights).
    float2 FullSize = float2(Scene().ScreenSize.xy);
    UV = (floor(UV * FullSize) + 0.5) / FullSize;

    float Raw = gTextures2D[DepthIndex].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], UV, 0.0).r;
    if (Raw <= 0.0)
    {
        return 0.0;   // sky sentinel
    }
    return ReconstructViewPos(UV, Raw, GetInverseCameraProjection()).z;
}

[shader("fragment")]
float4 FragmentMain(FragmentInput Input) : SV_Target0
{
    FData* Data = GetPassArgs<FData>();

    // Half-res texel: the AO image is ScreenSize / 2.
    float2 TexelSize = 2.0 / float2(Scene().ScreenSize.xy);

    float CenterZ = ViewDepth(Data.DepthIndex, Input.UV);
    if (CenterZ <= 0.0)
    {
        return float4(1.0, 1.0, 1.0, 1.0);   // sky
    }

    // Local depth slope per half-res pixel from quad derivatives, clamped so a silhouette in the
    // quad (or a sky neighbour) can't blow the plane open and bleed AO across the edge.
    float MaxSlope = CenterZ * 0.05;
    float Gx = clamp(ddx(CenterZ), -MaxSlope, MaxSlope);
    float Gy = clamp(ddy(CenterZ), -MaxSlope, MaxSlope);

    float InvTolerance = 1.0 / max(CenterZ * 0.02, 1e-3);

    float Sum = 0.0;
    float WSum = 0.0;
    for (int x = -2; x <= 2; ++x)
    {
        for (int y = -2; y <= 2; ++y)
        {
            float2 SUV = Input.UV + float2(x, y) * TexelSize;
            float  AO  = gTextures2D[Data.AOIndex].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], SUV, 0.0).r;
            float  Z   = ViewDepth(Data.DepthIndex, SUV);

            float ExpectedZ = CenterZ + Gx * float(x) + Gy * float(y);
            float Dz = (Z - ExpectedZ) * InvTolerance;
            float W  = (Z > 0.0) ? exp2(-Dz * Dz) : 0.0;
            Sum  += AO * W;
            WSum += W;
        }
    }

    float Result = (WSum > 1e-4) ? Sum / WSum : 1.0;
    return float4(Result, Result, Result, 1.0);
}
