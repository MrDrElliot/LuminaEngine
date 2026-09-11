#pragma once
#include "Includes/TextCommon.slang"

struct VertexOutput
{
    float2 UV                       : TEXCOORD0;
    nointerpolation float4 Color    : COLOR0;
    nointerpolation uint EntityID   : ENTITY_ID;
    float4 Position                 : SV_Position;
};

[shader("vertex")]
VertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    FGPUGlyph Glyph = SpanGet(&Pass().Glyphs, InstanceIndex);

    const float2 Corner = kTextQuadCorners[VertexIndex];

    // Position: lerp the quad bounds in the text plane, then map into world via the plane basis.
    const float2 Plane    = lerp(Glyph.PlaneMin, Glyph.PlaneMax, Corner);
    const float3 WorldPos = Glyph.Origin + Glyph.Right * Plane.x + Glyph.Up * Plane.y;

    // Atlas UV: row 0 is the top, so the plane-top corner (Corner.y == 1) maps to UVRect.y.
    const float U = lerp(Glyph.UVRect.x, Glyph.UVRect.z, Corner.x);
    const float V = lerp(Glyph.UVRect.w, Glyph.UVRect.y, Corner.y);

    VertexOutput Output;
    Output.UV       = float2(U, V);
    Output.Color    = UnpackColor(Glyph.Color);
    Output.EntityID = Glyph.EntityID;
    Output.Position = WorldToClip(WorldPos);
    return Output;
}
