#pragma once
#include "Includes/SceneGlobals.slang"

struct FragmentInput
{
    float2 UV                           : TEXCOORD0;
    float4 Tint                         : TINT;
    nointerpolation uint TextureIndex   : TEXTURE_INDEX;
    nointerpolation uint EntityID       : ENTITY_ID;
};

// Drawn after tone mapping onto the display-referred target, so RmlUi's sRGB colors are written as-is.
[shader("fragment")]
float4 FragmentMain(FragmentInput Input) : SV_Target0
{
    float4 Texel = FTexture2D(Input.TextureIndex).Sample(Samplers::LinearClamp, Input.UV) * Input.Tint;

    if (Texel.a < 0.01)
    {
        discard;
    }

    return Texel;
}
