#pragma once
#include "SceneGlobals.slang"
// Decals have a real tangent frame, so the graph's ParallaxOcclusionMapping node is legal here.
#include "ParallaxOcclusion.slang"
#include "Triplanar.slang"

// Unit-cube decals that project scene depth into decal-local space and write the DBuffer.

struct FGPUDecal
{
    float4x4 WorldToDecal;   // inside the box iff all(abs(local) <= 0.5)
    float4x4 DecalToWorld;   // entity transform, scale is the box extents
    float    FadeAngleCos;   // cos of the max angle between surface normal and decal forward, fading below it
    float    Opacity;        // master coverage multiplier
    uint     MaterialIndex;
    float    EmissionEnergy;
    float4   UVTransform;    // xy scale and zw offset of the atlas cell shown
    float4   Modulate;       // rgb tint, a albedo mix
    float4   Fades;          // x distance fade begin (negative disables), y its length, z upper fade, w lower fade
};

// Measured to the box center, so a decal fades as one piece rather than across its own footprint.
float DecalDistanceFade(FGPUDecal Decal, float3 CameraPosition)
{
    if (Decal.Fades.x < 0.0)
    {
        return 1.0;
    }
    float3 Center   = mul(Decal.DecalToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    float  Distance = length(CameraPosition - Center);
    return saturate((Decal.Fades.x + Decal.Fades.y - Distance) / max(Decal.Fades.y, 1e-4));
}

// The decal index arrives via SV_VulkanInstanceID (FirstInstance), and the pointer leads to stay 8-byte aligned.
struct FDecalPushConstants
{
    GPUSpan<FGPUDecal> Decals;
    FTexture2D         Depth;    // opaque scene depth (raw reverse-Z NDC [0,1])
    uint               _Pad;
};
FDecalPushConstants* Pass() { return GetPassArgs<FDecalPushConstants>(); }

float SampleDecalSceneDepth(float2 UV)
{
    return Pass().Depth.SampleLevel(Samplers::PointClamp, UV, 0.0).r;
}

// 12 triangles in local space [-0.5, 0.5], indexed by SV_VertexID.
static const float3 kDecalCubeVerts[36] = {
    // -X
    float3(-0.5,-0.5,-0.5), float3(-0.5,-0.5, 0.5), float3(-0.5, 0.5, 0.5),
    float3(-0.5,-0.5,-0.5), float3(-0.5, 0.5, 0.5), float3(-0.5, 0.5,-0.5),
    // +X
    float3( 0.5,-0.5,-0.5), float3( 0.5, 0.5, 0.5), float3( 0.5,-0.5, 0.5),
    float3( 0.5,-0.5,-0.5), float3( 0.5, 0.5,-0.5), float3( 0.5, 0.5, 0.5),
    // -Y
    float3(-0.5,-0.5,-0.5), float3( 0.5,-0.5,-0.5), float3( 0.5,-0.5, 0.5),
    float3(-0.5,-0.5,-0.5), float3( 0.5,-0.5, 0.5), float3(-0.5,-0.5, 0.5),
    // +Y
    float3(-0.5, 0.5,-0.5), float3( 0.5, 0.5, 0.5), float3( 0.5, 0.5,-0.5),
    float3(-0.5, 0.5,-0.5), float3(-0.5, 0.5, 0.5), float3( 0.5, 0.5, 0.5),
    // -Z
    float3(-0.5,-0.5,-0.5), float3(-0.5, 0.5,-0.5), float3( 0.5, 0.5,-0.5),
    float3(-0.5,-0.5,-0.5), float3( 0.5, 0.5,-0.5), float3( 0.5,-0.5,-0.5),
    // +Z
    float3(-0.5,-0.5, 0.5), float3( 0.5, 0.5, 0.5), float3(-0.5, 0.5, 0.5),
    float3(-0.5,-0.5, 0.5), float3( 0.5,-0.5, 0.5), float3( 0.5, 0.5, 0.5),
};
