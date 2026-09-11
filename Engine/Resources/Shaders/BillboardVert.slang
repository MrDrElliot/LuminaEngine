#pragma once
#include "Includes/SceneGlobals.slang"

static const float2 Positions[6] =
{
    float2(-1.0, -1.0),  // Triangle 1
    float2( 1.0, -1.0),
    float2( 1.0,  1.0),

    float2(-1.0, -1.0),  // Triangle 2
    float2( 1.0,  1.0),
    float2(-1.0,  1.0)
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
    float2 UV                                    : TEXCOORD0;
    nointerpolation float3 ColorTint             : COLORTINT;
    nointerpolation uint TextureIndex            : TEXTURE_INDEX;
    nointerpolation uint EntityID                : ENTITY_ID;
    float4 Position                              : SV_Position;
};

[shader("vertex")]
VertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    float2 QuadPos = Positions[VertexIndex];

    FBillboardInstance Billboard = SpanGet(Billboards(), InstanceIndex);

    float4x4 ViewMatrix = GetCameraView();

    float3 CameraRight_WS   = float3(ViewMatrix[0][0], ViewMatrix[0][1], ViewMatrix[0][2]);
    float3 CameraUp_WS      = -float3(ViewMatrix[1][0], ViewMatrix[1][1], ViewMatrix[1][2]);

    const float BillboardGrowStart = 15.0;
    const float MaxGrowth          = 12.0;

    float Distance   = distance(GetCameraPosition(), Billboard.Position);
    float GrowScale  = clamp(Distance / BillboardGrowStart, 1.0, MaxGrowth);
    float SizeScaled = Billboard.Size * GrowScale;

    float3 WorldPos = Billboard.Position
                    + CameraRight_WS * QuadPos.x * SizeScaled
                    + CameraUp_WS    * QuadPos.y * SizeScaled;

    VertexOutput Output;
    Output.UV           = UVs[VertexIndex];
    Output.ColorTint    = Billboard.ColorTint;
    Output.TextureIndex = Billboard.TextureIndex;
    Output.EntityID     = Billboard.EntityID;
    Output.Position     = mul(GetCameraProjection(), mul(GetCameraView(), float4(WorldPos, 1.0)));
    return Output;
}