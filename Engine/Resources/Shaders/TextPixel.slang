#pragma once
#include "Includes/TextCommon.slang"

struct FragmentInput
{
    float2 UV                       : TEXCOORD0;
    nointerpolation float4 Color    : COLOR0;
    nointerpolation uint EntityID   : ENTITY_ID;
};

struct FragmentOutput
{
    float4 Color  : SV_Target0;
    uint   Picker : SV_Target1;   // entity id stamped into the Picker buffer where the glyph is opaque
};

// Scales the per-texel MSDF range into screen pixels so the edge stays about 1px wide (Chlumsky's standard resolve).
float TextScreenPxRange(float2 UV)
{
    const float2 UnitRange     = float2(Pass().DistanceRange) / float2(Pass().AtlasWidth, Pass().AtlasHeight);
    const float2 ScreenTexSize = 1.0 / fwidth(UV);
    return max(0.5 * dot(UnitRange, ScreenTexSize), 1.0);
}

[shader("fragment")]
FragmentOutput FragmentMain(FragmentInput Input)
{
    // Alpha is a true SDF, so taking the inside-most of it and the median fills the median's dips at concave junctions.
    const float4 MSDF = Pass().Atlas.Sample(Samplers::LinearClamp, Input.UV);
    const float  SD = max(TextMedian(MSDF.r, MSDF.g, MSDF.b), MSDF.a);
    const float  Opacity = saturate((SD - 0.5) * TextScreenPxRange(Input.UV) + 0.5);

    if (Opacity <= 0.0)
    {
        discard;
    }

    FragmentOutput Output;
    Output.Color  = float4(Input.Color.rgb, Input.Color.a * Opacity);
    Output.Picker = Input.EntityID;
    return Output;
}
