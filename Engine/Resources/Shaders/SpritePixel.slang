#pragma once
#include "Includes/SpriteCommon.slang"

struct FragmentInput
{
    float2 UV                            : TEXCOORD0;
    nointerpolation float4 Color         : COLOR0;
    nointerpolation uint EntityID        : ENTITY_ID;
    nointerpolation uint Flags           : SPRITE_FLAGS;
    nointerpolation float AlphaCut       : ALPHA_CUT;
};

struct FragmentOutput
{
    float4 Color  : SV_Target0;
    uint   Picker : SV_Target1;   // entity id stamped where the sprite is visible
};

[shader("fragment")]
FragmentOutput FragmentMain(FragmentInput Input)
{
    const float4 Texel = gTextures2D[Pass().TextureIndex].Sample(gSamplers[SAMPLER_LINEAR_CLAMP], Input.UV);
    const float4 Shaded = Texel * Input.Color;

    if ((Input.Flags & SPRITE_FLAG_ALPHA_CUT) != 0u)
    {
        if (Shaded.a < Input.AlphaCut)
        {
            discard;
        }
    }
    else if (Shaded.a <= 0.0)
    {
        // Fully transparent fragments would still stamp the picker and claim clicks.
        discard;
    }

    FragmentOutput Output;
    Output.Color  = Shaded;
    Output.Picker = Input.EntityID;
    return Output;
}
