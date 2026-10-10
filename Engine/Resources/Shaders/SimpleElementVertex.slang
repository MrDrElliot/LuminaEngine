#pragma once
#include "Includes/SceneGlobals.slang"

// Pulled from a transient ring slice by device address. Matches FSimpleElementVertex in Vertex.h (16 B).
struct FSimpleElementVertex
{
    float3 Position;
    uint   Color;
};

struct FSimpleElementPass
{
    GPUSpan<FSimpleElementVertex> Vertices;
};
FSimpleElementPass* Pass() { return GetPassArgs<FSimpleElementPass>(); }

struct VSOutput
{
    float4 Position : SV_Position;
    float4 Color    : COLOR;
};

[shader("vertex")]
VSOutput VertexMain(uint VertexID : SV_VulkanVertexID)
{
    // SV_VulkanVertexID includes firstVertex, so a batch's StartVertexLocation indexes the whole transient array.
    FSimpleElementVertex V = Pass().Vertices.Get(VertexID);

    VSOutput Output;

    // Pad vertices (FImmediateLineRenderer::MakePadVertex) are infinite, and x > w puts the line wholly outside one clip plane.
    if (isinf(V.Position.x))
    {
        Output.Position = float4(2.0f, 2.0f, 2.0f, 1.0f);
        Output.Color    = float4(0.0f);
        return Output;
    }

    Output.Position = mul(GetCameraProjection(), mul(GetCameraView(), float4(V.Position, 1.0f)));
    Output.Color    = UnpackColor(V.Color);
    return Output;
}
