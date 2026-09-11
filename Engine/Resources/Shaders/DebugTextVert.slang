#pragma once
#include "Includes/TextCommon.slang"

// Screen-space debug text (World::DrawDebugText). Reuses FGPUGlyph, but PlaneMin/Max hold pixel coords
// (min = top-left, max = bottom-right); converts pixels -> NDC using the viewport size in the push const.

struct VertexOutput
{
    float2 UV                       : TEXCOORD0;
    nointerpolation float4 Color    : COLOR0;
    float4 Position                 : SV_Position;
};

[shader("vertex")]
VertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    FGPUGlyph Glyph = SpanGet(&Pass().Glyphs, InstanceIndex);

    const float2 Corner = kTextQuadCorners[VertexIndex];          // (0,0)=top-left .. (1,1)=bottom-right
    // Vulkan NDC is Y-down with a positive-height viewport (the Y-flip lives in the camera projection, which
    // screen-space text bypasses), so top-left pixel (0,0) maps to NDC (-1, -1).
    const float2 Px     = lerp(Glyph.PlaneMin, Glyph.PlaneMax, Corner);
    const float2 NDC    = float2(Px.x / float(Pass().ScreenWidth)  * 2.0 - 1.0,
                                 Px.y / float(Pass().ScreenHeight) * 2.0 - 1.0);

    // Atlas row 0 = glyph top, and Corner.y=0 is the top here, so V maps straight (no flip).
    const float U = lerp(Glyph.UVRect.x, Glyph.UVRect.z, Corner.x);
    const float V = lerp(Glyph.UVRect.y, Glyph.UVRect.w, Corner.y);

    VertexOutput Output;
    Output.UV       = float2(U, V);
    Output.Color    = UnpackColor(Glyph.Color);
    Output.Position = float4(NDC, 0.0, 1.0);
    return Output;
}
