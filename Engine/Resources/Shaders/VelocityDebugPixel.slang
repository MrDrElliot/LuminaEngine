#pragma once
#include "Includes/SceneGlobals.slang"

// Motion vectors as color. A still camera over a still scene reads flat mid gray everywhere.

struct FData
{
    uint VelocityIndex;
};

// Motion at this many pixels per frame saturates a channel.
static const float kRefPixels = 16.0;

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

[shader("fragment")]
float4 FragmentMain(FragmentInput Input) : SV_Target0
{
    FData* Data = GetPassArgs<FData>();

    const float2 Motion = gTextures2D[Data.VelocityIndex]
        .SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], Input.UV, 0.0).rg;

    const float2 Pixels = Motion * float2(Scene().ScreenSize.xy);
    const float2 Scaled = clamp(Pixels / kRefPixels, -1.0, 1.0);

    return float4(0.5 + 0.5 * Scaled.x, 0.5 + 0.5 * Scaled.y, 0.5, 1.0);
}
