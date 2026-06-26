#pragma once

//  Shader conventions for the RHI


struct FRHIRoot
{
    uint64_t Args;
};
[[vk::push_constant]] FRHIRoot gRHI;

// Per-pass args block reached through the single 8-byte push constant. Returns the
// pointer; field access auto-derefs, so pointer-bearing blocks are safe (no by-value
// load of PhysicalStorageBuffer pointers, which trips spirv-val).
Ptr<T> GetArgs<T>()
{
    return (T*)gRHI.Args;
}

// ---------------------------------------------------------------------------
//  Global texture heap.
// ---------------------------------------------------------------------------

[[vk::binding(0, 0)]] SamplerState                              gSamplers[];
// Comparison alias of the same sampler array (VkSampler carries compareEnable;
// only SAMPLER_SHADOW is created as a comparison sampler).
[[vk::binding(0, 0)]] SamplerComparisonState                    gCmpSamplers[];

[[vk::binding(1, 0)]] Texture2D                                 gTextures2D[];
[[vk::binding(1, 0)]] TextureCube                               gTexturesCube[];
[[vk::binding(1, 0)]] Texture3D                                 gTextures3D[];
[[vk::binding(1, 0)]] Texture2DArray                            gTextures2DArray[];
// Integer alias of the same heap, for R32_UINT targets (e.g. the VisBuffer triangle-ID buffer).
[[vk::binding(1, 0)]] Texture2D<uint>                           gTextures2DUInt[];

[[vk::binding(2, 0)]] RWTexture2D<float4>                       gRWTextures2D[];
[[vk::binding(2, 0)]] RWTexture3D<float4>                       gRWTextures3D[];
// Cube storage views are illegal in Vulkan; cube RW slots are registered as
// 2D-array views (HeapWriteRWTexture) and written via [int3(x, y, face)].
[[vk::binding(2, 0)]] RWTexture2DArray<float4>                  gRWTextures2DArray[];
// Coherent alias for cross-workgroup hand-offs (SPD-style single-pass chains).
[[vk::binding(2, 0)]] globallycoherent RWTexture2D<float4>      gRWTextures2DCoherent[];

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

// ---------------------------------------------------------------------------
//  Heap access helpers.
// ---------------------------------------------------------------------------

SamplerState GetSampler(uint SamplerIndex) { return gSamplers[SamplerIndex]; }

float4 SampleTexture2D(uint TextureIndex, uint SamplerIndex, float2 UV)
{
    return gTextures2D[TextureIndex].Sample(gSamplers[SamplerIndex], UV);
}

// Explicit-gradient sample. The deferred VisBuffer material lane has no rasterizer 2x2 quad (its quad
// straddles unrelated triangles), so implicit ddx/ddy are garbage -> wrong mips on every silhouette/seam.
// DeferredMaterial.slang feeds the analytic perspective-correct UV gradients here instead.
float4 SampleTexture2DGrad(uint TextureIndex, uint SamplerIndex, float2 UV, float2 Ddx, float2 Ddy)
{
    return gTextures2D[TextureIndex].SampleGrad(gSamplers[SamplerIndex], UV, Ddx, Ddy);
}

float4 SampleTexture2DLevel(uint TextureIndex, uint SamplerIndex, float2 UV, float Lod)
{
    return gTextures2D[TextureIndex].SampleLevel(gSamplers[SamplerIndex], UV, Lod);
}

float4 SampleTextureCube(uint TextureIndex, uint SamplerIndex, float3 Dir)
{
    return gTexturesCube[TextureIndex].Sample(gSamplers[SamplerIndex], Dir);
}

float4 SampleTextureCubeLevel(uint TextureIndex, uint SamplerIndex, float3 Dir, float Lod)
{
    return gTexturesCube[TextureIndex].SampleLevel(gSamplers[SamplerIndex], Dir, Lod);
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

// Fullscreen-triangle vertex helper: call from a VS with SV_VertexID, no
// vertex buffers. UV is top-left origin; depth lands on the far plane to
// match the engine's FullscreenQuad convention.
float4 FullscreenTriangle(uint VertexID, out float2 UV)
{
    UV = float2((VertexID << 1) & 2, VertexID & 2);
    return float4(UV * 2.0 - 1.0, 1.0, 1.0);
}
