#pragma once

//  Shader conventions for the RHI


struct FRHIRoot
{
    uint64_t Args;
};
[[vk::push_constant]] FRHIRoot gRHI;

// Returns the pointer; field access auto-derefs, so pointer-bearing blocks are safe (no by-value load
// of PhysicalStorageBuffer pointers, which trips spirv-val).
Ptr<T> GetArgs<T>()
{
    return (T*)gRHI.Args;
}

// Store nested addresses as uint64_t and go through this, never as a typed pointer field: GPU-AV
// instruments a typed load with a null of pointer type, which SPIR-V forbids, and disables itself.
T LoadBlock<T>(uint64_t Address)
{
    return ((T*)Address)[0];
}

// Global texture heap.

[[vk::binding(0, 0)]] SamplerState                              gSamplers[];
// Comparison alias of the same sampler array (VkSampler carries compareEnable;
// only SAMPLER_SHADOW is created as a comparison sampler).
[[vk::binding(0, 0)]] SamplerComparisonState                    gCmpSamplers[];

[[vk::binding(1, 0)]] Texture2D                                 gTextures2D[];
[[vk::binding(1, 0)]] TextureCube                               gTexturesCube[];
[[vk::binding(1, 0)]] Texture3D                                 gTextures3D[];
[[vk::binding(1, 0)]] Texture2DArray                            gTextures2DArray[];
// Reflection-probe radiance: every probe's 6 faces live in one array, sampled by slice index.
[[vk::binding(1, 0)]] TextureCubeArray                          gTexturesCubeArray[];
// Integer alias of the same heap, for R32_UINT targets (e.g. the VisBuffer triangle-ID buffer).
[[vk::binding(1, 0)]] Texture2D<uint>                           gTextures2DUInt[];

// The storage-image half (binding 2) lives in GlobalRHIStorage.slang, included only by shaders that
// write to the heap: its aliases disagree about view type and cost a validation error otherwise.

// Stock sampler heap slots. Keep in lockstep with the registration order in
// RHICore.cpp (RHI::Core::Initialize).
static const uint SAMPLER_LINEAR_WRAP   = 0;
static const uint SAMPLER_LINEAR_CLAMP  = 1;
static const uint SAMPLER_LINEAR_MIRROR = 2;
static const uint SAMPLER_POINT_WRAP    = 3;
static const uint SAMPLER_POINT_CLAMP   = 4;
static const uint SAMPLER_ANISO_WRAP    = 5;
static const uint SAMPLER_ANISO_CLAMP   = 6;
static const uint SAMPLER_SHADOW        = 7;
static const uint SAMPLER_MIN_REDUCTION = 8;
static const uint SAMPLER_MAX_REDUCTION = 9;

// Heap access helpers.

SamplerState GetSampler(uint SamplerIndex) { return gSamplers[SamplerIndex]; }

float4 SampleTexture2D(uint TextureIndex, uint SamplerIndex, float2 UV)
{
    return gTextures2D[TextureIndex].Sample(gSamplers[SamplerIndex], UV);
}

// Explicit-gradient sample. The deferred VisBuffer lane has no rasterizer quad, so implicit ddx/ddy
// are garbage -- DeferredMaterial.slang feeds analytic perspective-correct gradients here instead.
float4 SampleTexture2DGrad(uint TextureIndex, uint SamplerIndex, float2 UV, float2 Ddx, float2 Ddy)
{
    return gTextures2D[TextureIndex].SampleGrad(gSamplers[SamplerIndex], UV, Ddx, Ddy);
}

// Which sampling mode is right depends on the LANE, not the material: forward quads cover one
// triangle, the deferred lane's do not. One call shape lets the same emitted chunk serve every template.
float4 SampleTexture2DAuto(uint TextureIndex, uint SamplerIndex, float2 UV, float2 Ddx, float2 Ddy)
{
    return SampleTexture2D(TextureIndex, SamplerIndex, UV);
}

float4 SampleTexture2DLevel(uint TextureIndex, uint SamplerIndex, float2 UV, float Lod)
{
    return gTextures2D[TextureIndex].SampleLevel(gSamplers[SamplerIndex], UV, Lod);
}

// Slice is NOT filtered between layers -- the hardware treats the array index as discrete, which is
// the point. TextureIndex is the same ResourceID a Texture2D uses, but must name a Tex2DArray.
float4 SampleTexture2DArray(uint TextureIndex, uint SamplerIndex, float2 UV, uint Slice)
{
    return gTextures2DArray[TextureIndex].Sample(gSamplers[SamplerIndex], float3(UV, (float)Slice));
}

float4 SampleTexture2DArrayLevel(uint TextureIndex, uint SamplerIndex, float2 UV, uint Slice, float Lod)
{
    return gTextures2DArray[TextureIndex].SampleLevel(gSamplers[SamplerIndex], float3(UV, (float)Slice), Lod);
}

// Explicit-gradient variant, for the deferred VisBuffer lane (see SampleTexture2DGrad above). The
// gradients are 2D: a layer index has no derivative, so only UV drives mip selection.
float4 SampleTexture2DArrayGrad(uint TextureIndex, uint SamplerIndex, float2 UV, uint Slice, float2 Ddx, float2 Ddy)
{
    return gTextures2DArray[TextureIndex].SampleGrad(gSamplers[SamplerIndex], float3(UV, (float)Slice), Ddx, Ddy);
}

// Array counterpart of SampleTexture2DAuto, for the same reason. Without it an array sample reaches
// the deferred lane as implicit-derivative Sample(), which does not compile in a compute entry point.
float4 SampleTexture2DArrayAuto(uint TextureIndex, uint SamplerIndex, float2 UV, uint Slice, float2 Ddx, float2 Ddy)
{
    return SampleTexture2DArray(TextureIndex, SamplerIndex, UV, Slice);
}

float4 LoadTexture2DArray(uint TextureIndex, int2 Texel, uint Slice, int Mip = 0)
{
    return gTextures2DArray[TextureIndex].Load(int4(Texel, (int)Slice, Mip));
}

float4 SampleTextureCube(uint TextureIndex, uint SamplerIndex, float3 Dir)
{
    return gTexturesCube[TextureIndex].Sample(gSamplers[SamplerIndex], Dir);
}

float4 SampleTextureCubeLevel(uint TextureIndex, uint SamplerIndex, float3 Dir, float Lod)
{
    return gTexturesCube[TextureIndex].SampleLevel(gSamplers[SamplerIndex], Dir, Lod);
}

float4 SampleTextureCubeArrayLevel(uint TextureIndex, uint SamplerIndex, float3 Dir, uint Slice, float Lod)
{
    return gTexturesCubeArray[TextureIndex].SampleLevel(gSamplers[SamplerIndex], float4(Dir, (float)Slice), Lod);
}

float4 SampleTexture3D(uint TextureIndex, uint SamplerIndex, float3 UVW)
{
    return gTextures3D[TextureIndex].Sample(gSamplers[SamplerIndex], UVW);
}

float4 LoadTexture2D(uint TextureIndex, int2 Texel, int Mip = 0)
{
    return gTextures2D[TextureIndex].Load(int3(Texel, Mip));
}

uint LoadTexture2DUInt(uint TextureIndex, int2 Texel, int Mip = 0)
{
    return gTextures2DUInt[TextureIndex].Load(int3(Texel, Mip));
}

// Fullscreen-triangle vertex helper: call from a VS with SV_VertexID, no vertex buffers. UV is
// top-left origin; depth lands on the far plane to match the FullscreenQuad convention.
float4 FullscreenTriangle(uint VertexID, out float2 UV)
{
    UV = float2((VertexID << 1) & 2, VertexID & 2);
    return float4(UV * 2.0 - 1.0, 1.0, 1.0);
}
