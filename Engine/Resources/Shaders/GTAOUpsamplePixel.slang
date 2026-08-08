#pragma once
#include "Includes/SceneGlobals.slang"

// Joint bilateral upsample of the denoised half-res GTAO to full res: 3x3 half-res taps weighted
// by spatial Gaussian x depth-plane similarity (the tap's view depth vs the depth predicted by
// this full-res pixel's local slope). Re-aligns AO edges to full-res geometry -- a plain bilinear
// stretch stair-steps at creases, where the AO gradient is steepest.

struct FragmentInput
{
    float2 UV : TEXCOORD0;
};

struct FData
{
    uint AOIndex;      // bindless SRV of the denoised AO texture (half res)
    uint DepthIndex;   // bindless SRV of the full-res scene depth buffer
};

float ViewDepth(uint DepthIndex, float2 UV)
{
    // Half-res texel centers land on full-res texel corners; snap to a center so the point
    // sample doesn't flip-flop between texels. Identity for UVs already on full-res centers.
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

    float CenterZ = ViewDepth(Data.DepthIndex, Input.UV);
    if (CenterZ <= 0.0)
    {
        return float4(1.0, 1.0, 1.0, 1.0);   // sky
    }

    // Local depth slope per full-res pixel from quad derivatives, clamped so a silhouette in the
    // quad (or a sky neighbour) can't blow the plane open and bleed AO across the edge.
    float MaxSlope = CenterZ * 0.025;
    float Gx = clamp(ddx(CenterZ), -MaxSlope, MaxSlope);
    float Gy = clamp(ddy(CenterZ), -MaxSlope, MaxSlope);

    float InvTolerance = 1.0 / max(CenterZ * 0.02, 1e-3);

    uint2  HalfSize  = max(Scene().ScreenSize.xy >> 1u, uint2(1, 1));
    float2 HalfSizeF = float2(HalfSize);
    float2 FullSizeF = float2(Scene().ScreenSize.xy);

    // Nearest half-res texel to this full-res pixel.
    float2 NearestCoord = floor(Input.UV * HalfSizeF - 0.5 + 0.5);

    float Sum     = 0.0;
    float WSum    = 0.0;
    float Nearest = 1.0;   // fallback when every tap fails the depth test (full discontinuity)

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 TapCoord = clamp(NearestCoord + float2(x, y), float2(0.0), HalfSizeF - 1.0);
            float2 TapUV    = (TapCoord + 0.5) / HalfSizeF;

            float AO = gTextures2D[Data.AOIndex].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], TapUV, 0.0).r;
            float Z  = ViewDepth(Data.DepthIndex, TapUV);
            if (x == 0 && y == 0)
            {
                Nearest = AO;
            }

            // Spatial Gaussian over the offset in half-res texels.
            float2 DUVh = (TapUV - Input.UV) * HalfSizeF;
            float  Ws   = exp2(-dot(DUVh, DUVh));

            // Plane-predicted depth at the tap's full-res pixel offset.
            float2 DPix      = (TapUV - Input.UV) * FullSizeF;
            float  ExpectedZ = CenterZ + Gx * DPix.x + Gy * DPix.y;
            float  Dz        = (Z - ExpectedZ) * InvTolerance;
            float  Wd        = (Z > 0.0) ? exp2(-Dz * Dz) : 0.0;

            Sum  += AO * Ws * Wd;
            WSum += Ws * Wd;
        }
    }

    float Result = (WSum > 1e-4) ? Sum / WSum : Nearest;
    return float4(Result, Result, Result, 1.0);
}
