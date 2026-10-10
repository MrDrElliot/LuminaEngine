#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/XeGTAO.slang"

// Bilateral upsample of the half-resolution AO, weighting the four nearest texels by how well their depth matches this pixel's.

struct FPushConstants
{
    uint2        FullSize;
    uint2        HalfSize;
    FTexture2D   AO;
    FTexture2D   HalfDepth;
    FTexture2D   FullDepth;
    FRWTexture2D Output;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Relative depth difference at which a texel's weight halves; wide enough for slopes, narrow enough to stop halos at silhouettes.
static const float kDepthSigma = 0.05;

[shader("compute")]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 Pixel = DTid.xy;
    if (any(Pixel >= Pass().FullSize))
    {
        return;
    }

    const FGTAOView V   = MakeGTAOView(Pass().FullSize);
    const float     Raw = Pass().FullDepth.Load(int3(Pixel, 0)).r;
    const float     Z   = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(V, Raw));

    FTexture2D AO        = Pass().AO;
    FTexture2D HalfDepth = Pass().HalfDepth;

    const float2 HalfPos = (float2(Pixel) + 0.5) * (float2(Pass().HalfSize) / float2(Pass().FullSize)) - 0.5;
    const int2   Base    = int2(floor(HalfPos));
    const float2 F       = HalfPos - float2(Base);
    const int2   MaxTap  = int2(Pass().HalfSize) - 1;

    const float InvSigmaZ = 1.0 / (kDepthSigma * Z);

    float Sum       = 0.0;
    float SumWeight = 0.0;
    float Nearest   = 1.0;
    float NearestDz = 1e30;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const int2   Offset = int2(i & 1, i >> 1);
        const int3   Tap    = int3(clamp(Base + Offset, int2(0, 0), MaxTap), 0);
        const float  Value  = AO.Load(Tap).r;
        const float  Dz     = abs(HalfDepth.Load(Tap).r - Z);
        const float2 Bilin  = lerp(1.0 - F, F, float2(Offset));
        const float  Weight = Bilin.x * Bilin.y / (1.0 + Dz * InvSigmaZ);

        Sum       += Value * Weight;
        SumWeight += Weight;
        if (Dz < NearestDz)
        {
            NearestDz = Dz;
            Nearest   = Value;
        }
    }

    // A pixel no neighbor resembles, a thin sliver the reduced depth skipped, takes its closest match outright.
    const float Result = (NearestDz * InvSigmaZ > 4.0) ? Nearest : Sum / max(SumWeight, 1e-6);
    Pass().Output[Pixel] = float4(Result, 0.0, 0.0, 0.0);
}
