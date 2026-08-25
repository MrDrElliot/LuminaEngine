#pragma once
#include "Common.slang"
#include "GlobalRHI.slang"

// The deferred GBuffer contract -- nothing else may define this layout; a drifted channel silently
// reinterprets every surface. FULL at 20 B/pixel, so clearcoat BORROWS SelfShadow and Specular.

// Flags byte (GBufferC.a): bit 0 = receive shadow, bits 1-3 = shading model, bits 4-7 free.
#define GBUFFER_FLAG_RECEIVE_SHADOW 1u
#define GBUFFER_SHADING_MODEL_SHIFT 1u
#define GBUFFER_SHADING_MODEL_MASK  7u

// Three bits, so seven more models fit. Mirrored by EMaterialShadingModel (MaterialInterface.h) --
// keep the values in lockstep.
static const uint ShadingModel_Default   = 0u;
static const uint ShadingModel_Unlit     = 1u;
static const uint ShadingModel_Clearcoat = 2u;
// Two-sided thin translucency for leaves and grass. BORROWS the emissive channels, so foliage is not emissive.
static const uint ShadingModel_Foliage   = 3u;

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
    uint   ShadingModel;

    // Clearcoat model only; unread by every other model. See the channel-borrowing note above.
    float  Clearcoat;
    float  ClearcoatRoughness;

    // Foliage model only, borrowed from the emissive channels.
    float3 Transmission;        // colour the leaf passes through, already the lit tint the artist wants
    float  Thickness;           // 0 = opaque leaf, 1 = fully thin; scales the whole transmitted lobe
};

bool IsUnlit(FGBufferData G) { return G.ShadingModel == ShadingModel_Unlit; }

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
    G.Emissive           = float3(0.0, 0.0, 0.0);
    G.bReceiveShadow     = false;
    G.ShadingModel       = ShadingModel_Default;
    G.Clearcoat          = 0.0;
    G.ClearcoatRoughness = 0.03;
    G.Transmission       = float3(0.0, 0.0, 0.0);
    G.Thickness          = 0.0;
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
    Flags |= (G.ShadingModel & GBUFFER_SHADING_MODEL_MASK) << GBUFFER_SHADING_MODEL_SHIFT;

    const bool bClearcoat = (G.ShadingModel == ShadingModel_Clearcoat);
    const bool bFoliage   = (G.ShadingModel == ShadingModel_Foliage);

    // The borrowed channels. Written unconditionally from a select so the store stays branchless.
    const float ChannelBA = bClearcoat ? saturate(G.Clearcoat)          : saturate(G.SelfShadow);
    const float ChannelCB = bClearcoat ? saturate(G.ClearcoatRoughness) : saturate(G.Specular);
    const float3 ChannelD = bFoliage   ? saturate(G.Transmission)       : max(G.Emissive, float3(0.0, 0.0, 0.0));
    const float ChannelDA = bFoliage   ? saturate(G.Thickness)          : 0.0;

    FGBufferPacked P;
    P.A = float4(saturate(G.BaseColor), saturate(G.AmbientOcclusion));
    // Not normalized on store: the material pass already normalizes, and fp16 keeps it unit to well
    // within what the BRDF can distinguish.
    P.B = float4(G.WorldNormal, ChannelBA);
    P.C = float4(saturate(G.Roughness), saturate(G.Metallic), ChannelCB, (float)Flags / 255.0);
    P.D = float4(ChannelD, ChannelDA);
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
    G.bReceiveShadow   = (Flags & GBUFFER_FLAG_RECEIVE_SHADOW) != 0u;
    G.ShadingModel     = (Flags >> GBUFFER_SHADING_MODEL_SHIFT) & GBUFFER_SHADING_MODEL_MASK;

    // Undo the per-model borrow. The channels a model did NOT write get the value the lighting pass
    // would have used anyway, so nothing downstream has to know which model it is looking at.
    const bool bClearcoat = (G.ShadingModel == ShadingModel_Clearcoat);
    const bool bFoliage   = (G.ShadingModel == ShadingModel_Foliage);

    G.SelfShadow         = bClearcoat ? 1.0 : B.a;
    G.Specular           = bClearcoat ? 0.5 : C.b;
    G.Clearcoat          = bClearcoat ? B.a : 0.0;
    G.ClearcoatRoughness = bClearcoat ? C.b : 0.03;
    G.Emissive           = bFoliage ? float3(0.0, 0.0, 0.0) : D.rgb;
    G.Transmission       = bFoliage ? D.rgb : float3(0.0, 0.0, 0.0);
    G.Thickness          = bFoliage ? D.a   : 0.0;
    return G;
}

FGBufferData LoadGBuffer(FGBufferTargets T, int2 Texel)
{
    return UnpackGBuffer(LoadTexture2D(T.A, Texel),
                         LoadTexture2D(T.B, Texel),
                         LoadTexture2D(T.C, Texel),
                         LoadTexture2D(T.D, Texel));
}
