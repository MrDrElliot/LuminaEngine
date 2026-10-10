#pragma once
#include "Includes/SceneGlobals.slang"

// Writes the entity id pre-tone-map so widgets drawn post-tone-map stay click-selectable.

struct FragmentInput
{
    float2 UV                           : TEXCOORD0;
    float4 Tint                         : TINT;
    nointerpolation uint TextureIndex   : TEXTURE_INDEX;
    nointerpolation uint EntityID       : ENTITY_ID;
};

[shader("fragment")]
uint FragmentMain(FragmentInput Input) : SV_Target0
{
    const float Alpha = FTexture2D(Input.TextureIndex).Sample(Samplers::LinearClamp, Input.UV).a;
    if (Alpha < 0.01)
    {
        discard;
    }
    return Input.EntityID;
}
