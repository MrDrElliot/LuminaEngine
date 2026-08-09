#pragma once
#include "Common.slang"
#include "GlobalRHI.slang"

// The deferred GBuffer contract. The material pass writes it, the lighting pass and the debug-view pass
// read it, and nothing else may define its layout -- a drifted channel assignment reinterprets every
// surface silently rather than failing to compile.
//
//   GBufferA  RGBA8_UNORM      BaseColor.rgb                     | AmbientOcclusion
//   GBufferB  RGBA16_FLOAT     ShadingNormal.xyz (world)         | SelfShadow *
//   GBufferC  RGBA8_UNORM      Roughness | Metallic | Specular * | Flags
//   GBufferD  R11G11B10_FLOAT  Emissive.rgb
//
// (*) Reinterpreted per shading model. The buffer is otherwise FULL -- 20 B/pixel with no spare channel
// (D is R11G11B10, so it has no alpha) -- so a model needing extra parameters either adds a fifth target,
// paying bandwidth on every pixel whether it uses it or not, or borrows a channel that is meaningless for
// that model. Clearcoat does the latter:
//
//   Clearcoat:  B.a = ClearcoatFactor      (SelfShadow is parallax-only; forced to 1 for this model)
//               C.b = ClearcoatRoughness   (base Specular is pinned to the 0.5 dielectric default)
//
// Both borrows are safe because a coated surface is not also a parallax-occlusion surface, and coated
// dielectrics sit at the default F0. If either assumption ever stops holding, the fix is the fifth target.
//
// World position is NOT stored: it is reconstructed from the depth buffer the VisBuffer geometry pass
// already wrote, which is the same surface by construction.
//
// Every format here is either a mandatory Vulkan storage-image format or one the renderer already writes
// as a UAV elsewhere (R11G11B10 via the bloom chain), so the material pass can write all four from
// compute without a format-support query.

// Flags byte (GBufferC.a): bit 0 = receive shadow, bits 1-3 = shading model, bits 4-7 free.
#define GBUFFER_FLAG_RECEIVE_SHADOW 1u
#define GBUFFER_SHADING_MODEL_SHIFT 1u
#define GBUFFER_SHADING_MODEL_MASK  7u

// Which lighting path a pixel takes. Three bits, so seven more models fit before the flags byte runs out.
// Unlit is a model rather than the separate flag it used to be: two mechanisms for "how is this shaded"
// is exactly the kind of split that drifts.
//
// Mirrored by EMaterialShadingModel (MaterialInterface.h) -- keep the values in lockstep.
// Plain constants rather than a Slang enum: Slang scopes enum members, and these read better unqualified
// at every use site. Matches the MatFlag_* / SAMPLER_* style the other shared headers use.
static const uint ShadingModel_Default   = 0u;
static const uint ShadingModel_Unlit     = 1u;
static const uint ShadingModel_Clearcoat = 2u;

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

    // The two borrowed channels. Written unconditionally from a select so the store stays branchless.
    const float ChannelBA = bClearcoat ? saturate(G.Clearcoat)          : saturate(G.SelfShadow);
    const float ChannelCB = bClearcoat ? saturate(G.ClearcoatRoughness) : saturate(G.Specular);

    FGBufferPacked P;
    P.A = float4(saturate(G.BaseColor), saturate(G.AmbientOcclusion));
    // Not normalized on store: the material pass already normalizes, and fp16 keeps it unit to well
    // within what the BRDF can distinguish.
    P.B = float4(G.WorldNormal, ChannelBA);
    P.C = float4(saturate(G.Roughness), saturate(G.Metallic), ChannelCB, (float)Flags / 255.0);
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
    G.Emissive         = D.rgb;
    G.bReceiveShadow   = (Flags & GBUFFER_FLAG_RECEIVE_SHADOW) != 0u;
    G.ShadingModel     = (Flags >> GBUFFER_SHADING_MODEL_SHIFT) & GBUFFER_SHADING_MODEL_MASK;

    // Undo the per-model borrow. The channels a model did NOT write get the value the lighting pass
    // would have used anyway, so nothing downstream has to know which model it is looking at.
    const bool bClearcoat = (G.ShadingModel == ShadingModel_Clearcoat);

    G.SelfShadow         = bClearcoat ? 1.0 : B.a;
    G.Specular           = bClearcoat ? 0.5 : C.b;
    G.Clearcoat          = bClearcoat ? B.a : 0.0;
    G.ClearcoatRoughness = bClearcoat ? C.b : 0.03;
    return G;
}

FGBufferData LoadGBuffer(FGBufferTargets T, int2 Texel)
{
    return UnpackGBuffer(LoadTexture2D(T.A, Texel),
                         LoadTexture2D(T.B, Texel),
                         LoadTexture2D(T.C, Texel),
                         LoadTexture2D(T.D, Texel));
}
