#pragma once
#include "Includes/SceneGlobals.slang"

struct FragmentInput
{
    float2 UV                           : TEXCOORD0;
    float3 ColorTint                    : COLORTINT;
    nointerpolation uint TextureIndex   : TEXTURE_INDEX;
    nointerpolation uint EntityID       : ENTITY_ID;
};

struct FragmentOutput
{
    float4 Color  : SV_Target0;
    uint   Picker : SV_Target1;
};

[shader("fragment")]
FragmentOutput FragmentMain(FragmentInput Input)
{
    float4 Texture = FTexture2D(Input.TextureIndex).Sample(Samplers::LinearWrap, Input.UV);
    
    if (Texture.a < 0.01)
    {
        discard;
    }

    FragmentOutput Output;

    Output.Color  = Texture * float4(Input.ColorTint, 1.0);
    Output.Picker = Input.EntityID;
    return Output;
}