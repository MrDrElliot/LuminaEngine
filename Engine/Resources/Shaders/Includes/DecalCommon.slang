#pragma once
#include "SceneGlobals.slang"

// Shared bindings/data for the DecalPass (vertex + pixel). Decals are drawn as unit cubes; the pixel
// shader reconstructs world position from scene depth, projects into decal-local space, and writes the
// material attributes into the DBuffer. Set 0 (scene) gives camera + material uniforms + bindless
// textures. The pass binds NO private descriptor set: the per-decal array arrives by device address and
// the scene depth by bindless index, both carried in the push constant below.

struct FGPUDecal
{
    float4x4 WorldToDecal;   // world -> decal-local; inside the box iff all(abs(local) <= 0.5)
    float4x4 DecalToWorld;   // decal-local cube -> world (entity transform, scale = box extents)
    float    FadeAngleCos;   // cos(max angle) between surface normal and decal forward; below => fade
    float    Opacity;        // master coverage multiplier
    uint     MaterialIndex;  // index into Materials()
    uint     Flags;          // reserved
};

// Per-pass constants, identical in VS and PS so they share one push range. The decal *index* still
// arrives via SV_VulkanInstanceID (the draw's FirstInstance) -- only the array base + depth slot live
// here. Pointer first so the uint64 device address stays 8-byte aligned.
struct FDecalPushConstants
{
    FGPUDecal* Decals;       // device address of the per-frame decal array, indexed by decal index
    uint       DepthIndex;   // bindless resource ID of opaque scene depth (raw reverse-Z NDC [0,1])
    uint       _Pad;
};
FDecalPushConstants* Pass() { return GetPassArgs<FDecalPushConstants>(); }

// Opaque scene depth (raw reverse-Z NDC [0,1]) sampled point-clamped from the bindless table.
float SampleDecalSceneDepth(float2 UV)
{
    return gTextures2D[Pass().DepthIndex].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], UV, 0.0).r;
}

// Unit cube corner table (12 triangles / 36 verts), local space [-0.5, 0.5]. Indexed by SV_VertexID.
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
