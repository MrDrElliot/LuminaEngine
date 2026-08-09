#pragma once
#include "Common.slang"
#include "GlobalRHI.slang"

// The deferred GBuffer contract. The material pass writes it, the lighting pass and the debug-view pass
// read it, and nothing else may define its layout -- a drifted channel assignment reinterprets every
// surface silently rather than failing to compile.
//
//   GBufferA  RGBA8_UNORM      BaseColor.rgb                     | AmbientOcclusion
//   GBufferB  RGBA16_FLOAT     ShadingNormal.xyz (world)         | SelfShadow
//   GBufferC  RGBA8_UNORM      Roughness | Metallic | Specular   | Flags
//   GBufferD  R11G11B10_FLOAT  Emissive.rgb
//
// World position is NOT stored: it is reconstructed from the depth buffer the VisBuffer geometry pass
// already wrote, which is the same surface by construction.
//
// Every format here is either a mandatory Vulkan storage-image format or one the renderer already writes
// as a UAV elsewhere (R11G11B10 via the bloom chain), so the material pass can write all four from
// compute without a format-support query.

#define GBUFFER_FLAG_RECEIVE_SHADOW 1u
#define GBUFFER_FLAG_UNLIT          2u

// Bindless indices of the four targets. The material pass fills these with STORAGE slots and the readers
// with SAMPLED slots; the field order is the channel order above in both directions.
struct FGBufferTargets
{
    uint A;
    uint B;
    uint C;
    uint D;
};

struct FGBufferData
{
    float3 BaseColor;
    float  AmbientOcclusion;
    // Post normal-map, post-decal. The geometric normal does not survive into the GBuffer; the debug
    // views that want it re-reconstruct it from the VisBuffer.
    float3 WorldNormal;
    float  SelfShadow;
    float  Roughness;
    float  Metallic;
    float  Specular;
    float3 Emissive;
    bool   bReceiveShadow;
    bool   bUnlit;
};

FGBufferData DefaultGBufferData()
{
    FGBufferData G;
    G.BaseColor        = float3(0.0, 0.0, 0.0);
    G.AmbientOcclusion = 1.0;
    G.WorldNormal      = float3(0.0, 0.0, 1.0);
    G.SelfShadow       = 1.0;
    G.Roughness        = 1.0;
    G.Metallic         = 0.0;
    G.Specular         = 0.5;
    G.Emissive         = float3(0.0, 0.0, 0.0);
    G.bReceiveShadow   = false;
    G.bUnlit           = false;
    return G;
}

struct FGBufferPacked
{
    float4 A;
    float4 B;
    float4 C;
    float4 D;
};

FGBufferPacked PackGBuffer(FGBufferData G)
{
    uint Flags = 0u;
    Flags |= G.bReceiveShadow ? GBUFFER_FLAG_RECEIVE_SHADOW : 0u;
    Flags |= G.bUnlit         ? GBUFFER_FLAG_UNLIT          : 0u;

    FGBufferPacked P;
    P.A = float4(saturate(G.BaseColor), saturate(G.AmbientOcclusion));
    // Not normalized on store: the material pass already normalizes, and fp16 keeps it unit to well
    // within what the BRDF can distinguish.
    P.B = float4(G.WorldNormal, saturate(G.SelfShadow));
    P.C = float4(saturate(G.Roughness), saturate(G.Metallic), saturate(G.Specular), (float)Flags / 255.0);
    P.D = float4(max(G.Emissive, float3(0.0, 0.0, 0.0)), 0.0);
    return P;
}

FGBufferData UnpackGBuffer(float4 A, float4 B, float4 C, float4 D)
{
    const uint Flags = (uint)(C.a * 255.0 + 0.5);

    FGBufferData G;
    G.BaseColor        = A.rgb;
    G.AmbientOcclusion = A.a;
    G.WorldNormal      = normalize(B.xyz);
    G.SelfShadow       = B.a;
    G.Roughness        = C.r;
    G.Metallic         = C.g;
    G.Specular         = C.b;
    G.Emissive         = D.rgb;
    G.bReceiveShadow   = (Flags & GBUFFER_FLAG_RECEIVE_SHADOW) != 0u;
    G.bUnlit           = (Flags & GBUFFER_FLAG_UNLIT) != 0u;
    return G;
}

FGBufferData LoadGBuffer(FGBufferTargets T, int2 Texel)
{
    return UnpackGBuffer(LoadTexture2D(T.A, Texel),
                         LoadTexture2D(T.B, Texel),
                         LoadTexture2D(T.C, Texel),
                         LoadTexture2D(T.D, Texel));
}
