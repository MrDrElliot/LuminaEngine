// RmlUi vertex shader (fully-bindless, single-draw path). Vertices are pulled from the resident batch
// buffer by device address (Pass().Vertices); each carries a draw index into the per-draw device-address
// buffer (see RmlUiCommon), so the shader just transforms by the per-draw MVP.
#pragma once
#include "Includes/RmlUiCommon.slang"

struct VSOutput
{
    float4 Position : SV_Position;
    float4 Colour   : COLOR;
    float2 UV       : TEXCOORD0;
    nointerpolation uint DrawIndex : TEXCOORD1;
};

[shader("vertex")]
VSOutput VertexMain(uint VertexID : SV_VulkanVertexID)
{
    // Indices are rebased to VertexOffset 0, so SV_VulkanVertexID indexes the concatenated batch array.
    FUiVertex V    = Pass().Vertices[VertexID];
    FUiDraw   Draw = Pass().Draws[V.DrawIndex];

    VSOutput Output;
    Output.Position  = mul(Draw.MVP, float4(V.Position, 0.0, 1.0));
    Output.Colour    = float4( (V.Colour        & 0xFFu),
                               ((V.Colour >>  8) & 0xFFu),
                               ((V.Colour >> 16) & 0xFFu),
                               ((V.Colour >> 24) & 0xFFu)) / 255.0;
    Output.UV        = V.UV;
    Output.DrawIndex = V.DrawIndex;
    return Output;
}
