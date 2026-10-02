#pragma once
#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"

// Weighted blended OIT composite. Blend is One / SrcAlpha, so HDR = Average * (1 - Revealage) + HDR * Revealage.
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

    // View Mode > Translucency. Alpha 0 degenerates the composite blend to replace, so these overwrite the scene.
    uint DebugMode = Scene().CullData.DebugMode;
    if (DebugMode >= DEBUG_MODE_OIT_ACCUM_COLOR && DebugMode <= DEBUG_MODE_OIT_LAYER_COUNT)
    {
        float4 RawAccum = gTextures2D[Pass().AccumIndex].Load(int3(Coords, 0));

        float3 Debug;
        if (DebugMode == DEBUG_MODE_OIT_LAYER_COUNT)
        {
            // Hue-banded so a one-layer change reads as a color change, not a subtle gray step.
            uint  N   = (uint)(RawAccum.a + 0.5);
            float Hue = frac((float)N * 0.6180339887);
            float3 HueColor = saturate(float3(abs(Hue * 6.0 - 3.0) - 1.0,
                                              2.0 - abs(Hue * 6.0 - 2.0),
                                              2.0 - abs(Hue * 6.0 - 4.0)));
            Debug = (N == 0u) ? float3(0.0) : HueColor * (0.35 + 0.65 * saturate((float)N / 16.0));
        }
        else if (DebugMode == DEBUG_MODE_OIT_ACCUM_COLOR)
        {
            // Reinhard-compressed, with INF in red and NaN in magenta.
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
        else
        {
            Debug = float3(Revealage);
        }

        FSOutput DebugOut;
        DebugOut.Color = float4(Debug, 0.0);
        return DebugOut;
    }

    // Nothing translucent covered this pixel.
    if (Revealage >= 0.9999)
    {
        discard;
    }

    float4 Accum   = gTextures2D[Pass().AccumIndex].Load(int3(Coords, 0));
    float3 Average = Accum.rgb / max(Accum.a, 1e-5);

    // Drop the translucency rather than stamping INF into HDR for the tonemapper to spread.
    if (isinf(max3(abs(Average))) || any(Average != Average))
    {
        Average = float3(0.0);
    }

    FSOutput Output;
    Output.Color = float4(Average * (1.0 - Revealage), Revealage);
    return Output;
}
