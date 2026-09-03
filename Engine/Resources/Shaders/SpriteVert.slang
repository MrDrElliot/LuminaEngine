#pragma once
#include "Includes/SpriteCommon.slang"

struct VertexOutput
{
    float2 UV                            : TEXCOORD0;
    nointerpolation float4 Color         : COLOR0;
    nointerpolation uint EntityID        : ENTITY_ID;
    nointerpolation uint Flags           : SPRITE_FLAGS;
    nointerpolation float AlphaCut       : ALPHA_CUT;
    float4 Position                      : SV_Position;
};

[shader("vertex")]
VertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    FGPUSprite Sprite = Pass().Instances[InstanceIndex];

    const float2 Corner = kSpriteQuadCorners[VertexIndex];

    const float2 Plane    = lerp(Sprite.PlaneMin, Sprite.PlaneMax, Corner);
    const float3 WorldPos = Sprite.Origin + Sprite.Right * Plane.x + Sprite.Up * Plane.y;

    // Texture row 0 is the top, so the plane-top corner maps to UVRect.y.
    const float U = lerp(Sprite.UVRect.x, Sprite.UVRect.z, Corner.x);
    const float V = lerp(Sprite.UVRect.w, Sprite.UVRect.y, Corner.y);

    VertexOutput Output;
    Output.UV       = float2(U, V);
    Output.Color    = UnpackColor(Sprite.Color);
    Output.EntityID = Sprite.EntityID;
    Output.Flags    = Sprite.Flags;
    Output.AlphaCut = Sprite.AlphaCutThreshold;
    Output.Position = WorldToClip(WorldPos);
    return Output;
}
