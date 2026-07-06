#pragma once
#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"

// OIT resolve. No pass-local set: the WBOIT accum / revealage targets are read through the bindless
// table (texel fetch, no sampler) via indices in the push constant.
struct FPushConstants
{
    uint AccumIndex;
    uint RevealageIndex;
    uint _Pad0;
    uint _Pad1;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

struct FSInput
{
    float4 Coord : SV_Position;
}

struct FSOutput
{
    float4 Color : SV_Target0;
};

[shader("fragment")]
FSOutput FragmentMain(FSInput Input)
{
    int2 Coords = int2(Input.Coord.xy);

    float Revealage = gTextures2D[Pass().RevealageIndex].Load(int3(Coords, 0)).r;

    // OIT target inspectors (View Mode > Translucency): full-screen, opaque (alpha 1), no discard,
    // so the raw accumulation reads directly in the viewport -- including which channel strobes
    // frame to frame. Non-finite values are flagged in saturated colors (they are otherwise
    // masked by the isinf fallback below).
    uint DebugMode = Scene().CullData.DebugMode;
    if (DebugMode >= DEBUG_MODE_OIT_ACCUM_COLOR && DebugMode <= DEBUG_MODE_OIT_LAYER_COUNT)
    {
        float4 RawAccum = gTextures2D[Pass().AccumIndex].Load(int3(Coords, 0));

        float3 Debug;
        if (DebugMode == DEBUG_MODE_OIT_LAYER_COUNT)
        {
            // accum.a holds an exact fragment count (BasePixelPass writes 1.0 per fragment in this
            // mode). Hue-banded so a +-1 count flip reads as a COLOR change, not a subtle gray step;
            // brightness still ramps with depth of stack.
            uint  N   = (uint)(RawAccum.a + 0.5);
            float Hue = frac((float)N * 0.6180339887); // golden-ratio hue steps
            float3 HueColor = saturate(float3(abs(Hue * 6.0 - 3.0) - 1.0,
                                              2.0 - abs(Hue * 6.0 - 2.0),
                                              2.0 - abs(Hue * 6.0 - 4.0)));
            Debug = (N == 0u) ? float3(0.0) : HueColor * (0.35 + 0.65 * saturate((float)N / 16.0));
        }
        else if (DebugMode == DEBUG_MODE_OIT_ACCUM_COLOR)
        {
            // Reinhard-compressed accumulated (weighted, premultiplied) radiance.
            // INF = pure red, NaN = pure magenta.
            Debug = RawAccum.rgb / (1.0 + RawAccum.rgb);
            if (isinf(max3(abs(RawAccum.rgb))))
            {
                Debug = float3(1.0, 0.0, 0.0);
            }
            if (any(RawAccum.rgb != RawAccum.rgb))
            {
                Debug = float3(1.0, 0.0, 1.0);
            }
        }
        else if (DebugMode == DEBUG_MODE_OIT_ACCUM_WEIGHT)
        {
            // Weight sum (accum.a): grayscale ramp over ~[0, 2000]; red = approaching the fp16
            // ceiling (or already non-finite) -- red pixels mean the accumulation can overflow.
            float W = RawAccum.a;
            Debug = float3(W / (W + 2000.0));
            if (W > 32768.0 || isinf(W) || W != W)
            {
                Debug = float3(1.0, 0.0, 0.0);
            }
        }
        else // DEBUG_MODE_OIT_REVEALAGE
        {
            // Background transmittance product: white = no translucency, black = fully covered.
            Debug = float3(Revealage);
        }

        FSOutput DebugOut;
        DebugOut.Color = float4(Debug, 1.0);
        return DebugOut;
    }

    if(IsApproximatelyEqual(Revealage, 1.0, 1e-6))
    {
        discard;
    }

    float4 Accum = gTextures2D[Pass().AccumIndex].Load(int3(Coords, 0));

    if(isinf(max3(abs(Accum.rgb))))
    {
        Accum.rgb = float3(Accum.a);
    }

    // The accumulated rgb is Karis range-compressed per-surface radiance (BasePixelPass), weighted
    // identically to the alpha channel -- the ratio recovers the compressed average exactly, and
    // FireflyUntonemap restores HDR range. For a single surface this reconstructs Color * a +
    // Reflection over Background * (1 - Coverage) exactly.
    float3 FinalColor = FireflyUntonemap(Accum.rgb / max(Accum.a, 1e-6));

    FSOutput Output;
    Output.Color = float4(FinalColor, 1.0 - Revealage);
    return Output;
}
