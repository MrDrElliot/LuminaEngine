#pragma once
#include "Includes/TextCommon.slang"

struct FragmentInput
{
    float2 UV                       : TEXCOORD0;
    nointerpolation float4 Color    : COLOR0;
};

float DebugTextScreenPxRange(float2 UV)
{
    const float2 UnitRange     = float2(Pass().DistanceRange) / float2(Pass().AtlasWidth, Pass().AtlasHeight);
    const float2 ScreenTexSize = 1.0 / fwidth(UV);
    return max(0.5 * dot(UnitRange, ScreenTexSize), 1.0);
}

[shader("fragment")]
float4 FragmentMain(FragmentInput Input) : SV_Target0
{
    const float4 MSDF = Pass().Atlas.Sample(Samplers::LinearClamp, Input.UV);
    const float  SD = max(TextMedian(MSDF.r, MSDF.g, MSDF.b), MSDF.a);
    const float  Opacity = saturate((SD - 0.5) * DebugTextScreenPxRange(Input.UV) + 0.5);

    if (Opacity <= 0.0)
    {
        discard;
    }
    return float4(Input.Color.rgb, Input.Color.a * Opacity);
}
