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

// Reads a block whose address is stored INSIDE another args block, given that address as a
// plain integer.
//
// Store nested addresses as uint64_t and go through this, never as a typed field. A typed
// pointer field has to be loaded by value before it can be followed, and GPU-AV instruments
// that load with a bounds check whose failure path needs a null of the loaded type -- for a
// PhysicalStorageBuffer pointer that is OpConstantNull of a pointer type, which the SPIR-V
// spec forbids. The layer emits it anyway, fails its own spirv-val, and disables itself
// mid-run. Loading an integer and casting afterwards leaves nothing for it to null.
T LoadBlock<T>(uint64_t Address)
{
    return ((T*)Address)[0];
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
// Reflection-probe radiance: every probe's 6 faces live in one array, sampled by slice index.
[[vk::binding(1, 0)]] TextureCubeArray                          gTexturesCubeArray[];
// Integer alias of the same heap, for R32_UINT targets (e.g. the VisBuffer triangle-ID buffer).
[[vk::binding(1, 0)]] Texture2D<uint>                           gTextures2DUInt[];

// The storage-image half of the heap (binding 2) lives in GlobalRHIStorage.slang, which
// a shader includes only if it writes to the heap. See that file for why: the aliases
// there disagree about view type, so declaring them costs a validation error in any
// module that does not use them.

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

// Material-codegen texture sample. The compiler propagates analytic UV derivatives through the graph and
// passes them here; which of the two sampling modes is right depends on the LANE, not the material:
//
//   * Forward lanes (base pass, masked VisBuffer, terrain): the rasterizer's 2x2 quad covers ONE triangle,
//     so implicit derivatives are exact and cheaper. The gradient arguments go unused and dead-strip.
//   * Deferred VisBuffer pass: a quad there straddles unrelated triangles, so implicit derivatives are
//     garbage. DeferredMaterial.slang #defines this name to the SampleTexture2DGrad form above.
//
// Keeping one call shape in the codegen is what lets the same emitted pixel chunk serve every template.
float4 SampleTexture2DAuto(uint TextureIndex, uint SamplerIndex, float2 UV, float2 Ddx, float2 Ddy)
{
    return SampleTexture2D(TextureIndex, SamplerIndex, UV);
}

float4 SampleTexture2DLevel(uint TextureIndex, uint SamplerIndex, float2 UV, float Lod)
{
    return gTextures2D[TextureIndex].SampleLevel(gSamplers[SamplerIndex], UV, Lod);
}

// Array-texture sampling. Slice selects the layer and is NOT filtered between layers -- the hardware
// treats the array index as discrete, which is the whole point (layers are independent images, so
// blending across them would bleed unrelated content). Round/clamp before calling if the caller
// computes it. The heap arrays are aliased views of one descriptor array, so TextureIndex is the same
// ResourceID a Texture2D would use; it just has to name a texture actually created as Tex2DArray.
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

// Array counterpart of SampleTexture2DAuto, and it exists for the same reason: the material codegen emits
// one call shape and the DEFERRED template redirects it to the gradient form. Without it an array sample
// reached the deferred lane as an implicit-derivative Sample(), which is not merely wrong there -- it does
// not compile, because a compute entry point has no derivative quad.
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

// Fullscreen-triangle vertex helper: call from a VS with SV_VertexID, no
// vertex buffers. UV is top-left origin; depth lands on the far plane to
// match the engine's FullscreenQuad convention.
float4 FullscreenTriangle(uint VertexID, out float2 UV)
{
    UV = float2((VertexID << 1) & 2, VertexID & 2);
    return float4(UV * 2.0 - 1.0, 1.0, 1.0);
}
