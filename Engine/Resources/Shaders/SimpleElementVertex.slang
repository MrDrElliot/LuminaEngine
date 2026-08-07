#pragma once
#include "Includes/SceneGlobals.slang"

// Debug line/triangle vertices are pulled from a transient ring slice by device address
// instead of a bound vertex buffer -- no input layout, no persistent vertex buffer. Layout matches
// FSimpleElementVertex in Vertex.h (float3 + packed uint, 16 B).
struct FSimpleElementVertex
{
    float3 Position;
    uint   Color;
};

struct FSimpleElementPass
{
    FSimpleElementVertex* Vertices;
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
    // SV_VulkanVertexID is the raw gl_VertexIndex (firstVertex + i), so a per-batch StartVertexLocation
    // indexes straight into the whole transient vertex array.
    FSimpleElementVertex V = Pass().Vertices[VertexID];

    VSOutput Output;

    // Padding vertex (FImmediateLineRenderer::MakePadVertex): a thread's tail reservation is filled
    // with infinite positions rather than left holding whatever the buffer held three frames ago.
    // x > w on every vertex of the line puts it wholly outside one clip plane, so the clipper rejects
    // the primitive outright and it costs no fragments.
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
