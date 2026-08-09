#pragma once
#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"
#include "Includes/MomentOIT.slang"

// MBOIT composite. Every fragment was already weighted by the transmittance in front of it, so Accum.rgb
// is added, not averaged; the background is attenuated by exp(-b_0). Blend is One / SrcAlpha, hence the
// total transmittance in alpha:  HDR = Accum.rgb + HDR * exp(-b_0).
//
// Targets are read through the bindless table (texel fetch, no sampler) via the push constant.
struct FPushConstants
{
    uint AccumIndex;
    uint MomentZerothIndex;
    uint MomentsIndex;
    uint _Pad0;
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

    float B0 = gTextures2D[Pass().MomentZerothIndex].Load(int3(Coords, 0)).r;

    // View Mode > Translucency. Alpha 0 degenerates the composite blend to replace, so these overwrite
    // the scene rather than tinting it.
    uint DebugMode = Scene().CullData.DebugMode;
    if (DebugMode >= DEBUG_MODE_OIT_ACCUM_COLOR && DebugMode <= DEBUG_MODE_OIT_LAYER_COUNT)
    {
        float4 RawAccum = gTextures2D[Pass().AccumIndex].Load(int3(Coords, 0));

        float3 Debug;
        if (DebugMode == DEBUG_MODE_OIT_LAYER_COUNT)
        {
            // Hue-banded so a +-1 count flip reads as a colour change, not a subtle gray step.
            uint  N   = (uint)(RawAccum.a + 0.5);
            float Hue = frac((float)N * 0.6180339887);
            float3 HueColor = saturate(float3(abs(Hue * 6.0 - 3.0) - 1.0,
                                              2.0 - abs(Hue * 6.0 - 2.0),
                                              2.0 - abs(Hue * 6.0 - 4.0)));
            Debug = (N == 0u) ? float3(0.0) : HueColor * (0.35 + 0.65 * saturate((float)N / 16.0));
        }
        else if (DebugMode == DEBUG_MODE_OIT_ACCUM_COLOR)
        {
            // Reinhard-compressed. INF = red, NaN = magenta.
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
        else if (DebugMode == DEBUG_MODE_OIT_MOMENTS)
        {
            // Normalized b_1..b_3 as RGB. A flat single-colour field means every layer sits at one
            // depth -- the case MOMENT_BIAS exists for.
            if (B0 < MOMENT_MIN_ABSORBANCE)
            {
                Debug = float3(0.0);
            }
            else
            {
                float4 B = gTextures2D[Pass().MomentsIndex].Load(int3(Coords, 0)) / B0;
                Debug = any(B != B) ? float3(1.0, 0.0, 1.0) : saturate(abs(B.xyz));
            }
        }
        else // DEBUG_MODE_OIT_TRANSMITTANCE
        {
            Debug = float3(MomentTotalTransmittance(B0));
        }

        FSOutput DebugOut;
        DebugOut.Color = float4(Debug, 0.0);
        return DebugOut;
    }

    if (B0 < MOMENT_MIN_ABSORBANCE)
    {
        discard;
    }

    float4 Accum = gTextures2D[Pass().AccumIndex].Load(int3(Coords, 0));

    // Drop the translucency rather than stamping INF into HDR for the tonemapper to spread.
    if (isinf(max3(abs(Accum.rgb))) || any(Accum.rgb != Accum.rgb))
    {
        Accum.rgb = float3(0.0);
    }

    FSOutput Output;
    Output.Color = float4(Accum.rgb, MomentTotalTransmittance(B0));
    return Output;
}
