#pragma once
#include "Includes/SceneGlobals.slang"

// Quad geometry (two triangles), local XY plane in [-0.5, 0.5].
static const float2 Positions[6] =
{
    float2(-0.5, -0.5),
    float2( 0.5, -0.5),
    float2( 0.5,  0.5),

    float2(-0.5, -0.5),
    float2( 0.5,  0.5),
    float2(-0.5,  0.5)
};

static const float2 UVs[6] =
{
    float2(0.0, 0.0),
    float2(1.0, 0.0),
    float2(1.0, 1.0),

    float2(0.0, 0.0),
    float2(1.0, 1.0),
    float2(0.0, 1.0)
};

struct VertexOutput
{
    float2 UV                            : TEXCOORD0;
    nointerpolation float4 Tint          : TINT;
    nointerpolation uint TextureIndex    : TEXTURE_INDEX;
    nointerpolation uint EntityID        : ENTITY_ID;
    float4 Position                      : SV_Position;
};

[shader("vertex")]
VertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    FWidgetInstance Widget = SpanGet(Widgets(), InstanceIndex);

    const float2 QuadPos = Positions[VertexIndex];
    const float2 Scaled  = QuadPos * Widget.WorldSize;

    float3 WorldPos;
    if ((Widget.Flags & WIDGET_FLAG_BILLBOARD) != 0u)
    {
        // Camera-facing: place at the entity's translation, expand along view axes.
        const float3 Origin = mul(Widget.Transform, float4(0.0, 0.0, 0.0, 1.0)).xyz;

        const float4x4 View = GetCameraView();
        const float3 Right_WS = float3(View[0][0], View[0][1], View[0][2]);
        const float3 Up_WS    = -float3(View[1][0], View[1][1], View[1][2]);

        WorldPos = Origin + Right_WS * Scaled.x + Up_WS * Scaled.y;
    }
    else
    {
        // Oriented: quad lies in the entity's local XY plane (normal = local +Z). Negate Y so
        // the UI's top-left origin maps upright, matching the billboard path's -View[1] up.
        WorldPos = mul(Widget.Transform, float4(Scaled.x, -Scaled.y, 0.0, 1.0)).xyz;
    }

    VertexOutput Output;
    Output.UV           = UVs[VertexIndex];
    Output.Tint         = Widget.Tint;
    Output.TextureIndex = Widget.TextureIndex;
    Output.EntityID     = Widget.EntityID;
    Output.Position     = mul(GetCameraProjection(), mul(GetCameraView(), float4(WorldPos, 1.0)));
    return Output;
}
