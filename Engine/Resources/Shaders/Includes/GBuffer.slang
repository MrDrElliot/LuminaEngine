#pragma once
#include "SceneGlobals.slang"

// The single definition of the GBuffer layout, and at 16 B/pixel clearcoat borrows SelfShadow and Specular.

// GBufferC.a flags, bit 0 receive shadow, bits 1-3 shading model, bits 4-7 foliage and subsurface thickness.
#define GBUFFER_FLAG_RECEIVE_SHADOW 1u
#define GBUFFER_SHADING_MODEL_SHIFT 1u
#define GBUFFER_SHADING_MODEL_MASK  7u
#define GBUFFER_THICKNESS_SHIFT     4u
#define GBUFFER_THICKNESS_MASK      15u
// Models with no thickness spend bit 4 saying GBufferE and GBufferF hold this pixel's extended lobes.
#define GBUFFER_FLAG_EXTENDED       16u

// Mirrored by EMaterialShadingModel (MaterialInterface.h), so keep the values in lockstep.
static const uint ShadingModel_Default   = 0u;
static const uint ShadingModel_Unlit     = 1u;
static const uint ShadingModel_Clearcoat = 2u;
// Two-sided thin translucency for leaves and grass that borrows the emissive channels, so foliage is not emissive.
static const uint ShadingModel_Foliage   = 3u;
// Light scattered inside skin, wax or jade, carried in the same channels foliage borrows.
static const uint ShadingModel_Subsurface = 4u;

// Thickness rides the flag bits, so the extended lobes cannot share a pixel with it.
bool ShadingModelCarriesThickness(uint Model) { return Model == ShadingModel_Foliage || Model == ShadingModel_Subsurface; }

// r11g11b10 carries no alpha, so thickness rides the spare flag bits
uint PackThickness01(float Thickness) { return (uint)(saturate(Thickness) * 15.0 + 0.5); }

// The material pass fills these with storage slots and the readers with sampled slots.
struct FGBufferTargets
{
    FTexture2D A;
    FTexture2D B;
    FTexture2D C;
    FTexture2D D;
};

struct FGBufferData
{
    float3 BaseColor;
    float  AmbientOcclusion;
    // Post normal-map and post-decal, so debug views wanting the geometric normal rebuild it from the VisBuffer.
    float3 WorldNormal;
    float  SelfShadow;
    float  Roughness;
    float  Metallic;
    float  Specular;
    float3 Emissive;
    bool   bReceiveShadow;
    bool   bContactShadows;
    uint   ShadingModel;

    // Clearcoat model only, borrowed from SelfShadow and Specular.
    float  Clearcoat;
    float  ClearcoatRoughness;
    // The base layer's normal beneath the coat, which flakes or a weave tilt while the coat stays smooth.
    float3 BottomNormal;

    // Foliage and subsurface models only, borrowed from the emissive channels.
    float3 Transmission;        // color the leaf passes through, or the color subsurface light picks up inside
    float  Thickness;           // foliage 0 opaque to 1 fully thin, subsurface how far light spreads, quantized to 4 bits

    // Extended lobes, stored in GBufferE and GBufferF only where bExtended is set.
    bool   bExtended;
    float  Anisotropy;          // -1 stretches the highlight along the bitangent, 1 along the tangent
    float3 Tangent;             // unit, in the plane of WorldNormal
    float  HazeWeight;          // share of the specular in the second, rougher lobe
    float  HazeRoughness;
    float3 Fuzz;                // fuzz color times its amount
    float  FuzzRoughness;
    float  ThinFilmThickness;   // micrometers, 0 for no film
    float  ThinFilmIOR;
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
    G.bContactShadows    = true;
    G.ShadingModel       = ShadingModel_Default;
    G.Clearcoat          = 0.0;
    G.ClearcoatRoughness = 0.03;
    G.BottomNormal       = float3(0.0, 0.0, 1.0);
    G.Transmission       = float3(0.0, 0.0, 0.0);
    G.Thickness          = 0.0;
    G.bExtended          = false;
    G.Anisotropy         = 0.0;
    G.Tangent            = float3(1.0, 0.0, 0.0);
    G.HazeWeight         = 0.0;
    G.HazeRoughness      = 0.5;
    G.Fuzz               = float3(0.0, 0.0, 0.0);
    G.FuzzRoughness      = 0.5;
    G.ThinFilmThickness  = 0.0;
    G.ThinFilmIOR        = 1.5;
    return G;
}

// Thin film thickness the 8 bits span, past which the film is many wavelengths thick and its colors wash out.
static const float kThinFilmMaxMicrons = 1.2;
static const float kThinFilmMinIOR     = 1.0;
static const float kThinFilmMaxIOR     = 2.5;

// Any lobe beyond the base, which is what decides whether a pixel pays for the extension targets.
bool HasExtendedLobes(FGBufferData G)
{
    const bool bBottomNormal = G.ShadingModel == ShadingModel_Clearcoat && dot(G.BottomNormal, G.WorldNormal) < 0.9999;
    return abs(G.Anisotropy) > 1e-3 || G.HazeWeight > 1e-3 || any(G.Fuzz > 1e-3) || G.ThinFilmThickness > 1e-3 || bBottomNormal;
}

// A tangent angle needs a reference that depends on the normal alone, so the reader rebuilds the same one.
void GBufferTangentBasis(float3 N, out float3 B0, out float3 B1)
{
    // Frisvad 2012 with the singular pole handled, continuous everywhere except the south pole where it flips.
    if (N.z < -0.9999999)
    {
        B0 = float3(0.0, -1.0, 0.0);
        B1 = float3(-1.0, 0.0, 0.0);
        return;
    }
    const float A = 1.0 / (1.0 + N.z);
    const float B = -N.x * N.y * A;
    B0 = float3(1.0 - N.x * N.x * A, B, -N.x);
    B1 = float3(B, 1.0 - N.y * N.y * A, -N.y);
}

uint PackNibbles(float High, float Low)
{
    return ((uint)(saturate(High) * 15.0 + 0.5) << 4u) | (uint)(saturate(Low) * 15.0 + 0.5);
}

struct FGBufferExtension
{
    float4 E;   // r anisotropy, g tangent angle over pi, b haze weight and roughness nibbles, a thin film thickness
    float4 F;   // rgb fuzz times amount, a fuzz roughness and thin film IOR nibbles
};

FGBufferExtension PackGBufferExtension(FGBufferData G)
{
    float3 B0, B1;
    GBufferTangentBasis(G.WorldNormal, B0, B1);
    // A tangent and its negation stretch the highlight the same way, so half a turn covers every direction.
    float Angle = atan2(dot(G.Tangent, B1), dot(G.Tangent, B0));
    Angle = Angle < 0.0 ? Angle + PI : Angle;

    FGBufferExtension X;
    X.E = float4(saturate(G.Anisotropy * 0.5 + 0.5),
                 saturate(Angle / PI),
                 (float)PackNibbles(G.HazeWeight, G.HazeRoughness) / 255.0,
                 saturate(G.ThinFilmThickness / kThinFilmMaxMicrons));
    X.F = float4(saturate(G.Fuzz),
                 (float)PackNibbles(G.FuzzRoughness, (G.ThinFilmIOR - kThinFilmMinIOR) / (kThinFilmMaxIOR - kThinFilmMinIOR)) / 255.0);

    // Car paint has no fuzz, so a clearcoat pixel spends those channels on its bottom normal, kept relative to the normal for precision.
    if (G.ShadingModel == ShadingModel_Clearcoat)
    {
        X.F.rg = float2(dot(G.BottomNormal, B0), dot(G.BottomNormal, B1)) * 0.5 + 0.5;
        X.F.b  = 0.0;
    }
    return X;
}

void UnpackGBufferExtension(inout FGBufferData G, float4 E, float4 F)
{
    const uint Haze = (uint)(E.b * 255.0 + 0.5);
    const uint Fz   = (uint)(F.a * 255.0 + 0.5);

    float3 B0, B1;
    GBufferTangentBasis(G.WorldNormal, B0, B1);
    const float Angle = E.g * PI;

    G.Anisotropy        = E.r * 2.0 - 1.0;
    G.Tangent           = B0 * cos(Angle) + B1 * sin(Angle);
    G.HazeWeight        = (float)(Haze >> 4u) / 15.0;
    G.HazeRoughness     = (float)(Haze & 15u) / 15.0;
    G.ThinFilmThickness = E.a * kThinFilmMaxMicrons;
    G.Fuzz              = F.rgb;
    G.FuzzRoughness     = (float)(Fz >> 4u) / 15.0;
    G.ThinFilmIOR       = lerp(kThinFilmMinIOR, kThinFilmMaxIOR, (float)(Fz & 15u) / 15.0);

    if (G.ShadingModel == ShadingModel_Clearcoat)
    {
        const float2 Tilt = F.rg * 2.0 - 1.0;
        G.BottomNormal = normalize(B0 * Tilt.x + B1 * Tilt.y + G.WorldNormal * sqrt(saturate(1.0 - dot(Tilt, Tilt))));
        G.Fuzz         = float3(0.0, 0.0, 0.0);
    }
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
    const bool bClearcoat = (G.ShadingModel == ShadingModel_Clearcoat);
    const bool bFoliage   = ShadingModelCarriesThickness(G.ShadingModel);

    uint Flags = 0u;
    Flags |= G.bReceiveShadow ? GBUFFER_FLAG_RECEIVE_SHADOW : 0u;
    Flags |= (G.ShadingModel & GBUFFER_SHADING_MODEL_MASK) << GBUFFER_SHADING_MODEL_SHIFT;
    Flags |= (bFoliage ? PackThickness01(G.Thickness) : 0u) << GBUFFER_THICKNESS_SHIFT;
    Flags |= (!bFoliage && G.bExtended) ? GBUFFER_FLAG_EXTENDED : 0u;

    // The borrowed channels. Written unconditionally from a select so the store stays branchless.
    const float ChannelBB = bClearcoat ? saturate(G.Clearcoat)          : saturate(G.SelfShadow);
    const float ChannelCB = bClearcoat ? saturate(G.ClearcoatRoughness) : saturate(G.Specular);
    const float3 ChannelD = bFoliage   ? saturate(G.Transmission)       : max(G.Emissive, float3(0.0, 0.0, 0.0));

    FGBufferPacked P;
    P.A = float4(saturate(G.BaseColor), saturate(G.AmbientOcclusion));
    // Stored inverted, so a writer that leaves the alpha at zero keeps contact shadows on.
    P.B = float4(OctEncode01(G.WorldNormal), ChannelBB, G.bContactShadows ? 0.0 : 1.0);
    P.C = float4(saturate(G.Roughness), saturate(G.Metallic), ChannelCB, (float)Flags / 255.0);
    P.D = float4(ChannelD, 0.0);
    return P;
}

FGBufferData UnpackGBuffer(float4 A, float4 B, float4 C, float4 D)
{
    const uint Flags = (uint)(C.a * 255.0 + 0.5);

    FGBufferData G = DefaultGBufferData();
    G.BaseColor        = A.rgb;
    G.AmbientOcclusion = A.a;
    G.WorldNormal      = OctDecode01(B.rg);
    G.BottomNormal     = G.WorldNormal;
    G.Roughness        = C.r;
    G.Metallic         = C.g;
    G.bReceiveShadow   = (Flags & GBUFFER_FLAG_RECEIVE_SHADOW) != 0u;
    G.bContactShadows  = B.a < 0.5;
    G.ShadingModel     = (Flags >> GBUFFER_SHADING_MODEL_SHIFT) & GBUFFER_SHADING_MODEL_MASK;

    // Unborrowed channels get the value lighting would use anyway, so readers need not know the model.
    const bool bClearcoat = (G.ShadingModel == ShadingModel_Clearcoat);
    const bool bFoliage   = ShadingModelCarriesThickness(G.ShadingModel);
    G.bExtended = !bFoliage && (Flags & GBUFFER_FLAG_EXTENDED) != 0u;

    G.SelfShadow         = bClearcoat ? 1.0 : B.b;
    G.Specular           = bClearcoat ? 0.5 : C.b;
    G.Clearcoat          = bClearcoat ? B.b : 0.0;
    G.ClearcoatRoughness = bClearcoat ? C.b : 0.03;
    G.Emissive           = bFoliage ? float3(0.0, 0.0, 0.0) : D.rgb;
    G.Transmission       = bFoliage ? D.rgb : float3(0.0, 0.0, 0.0);
    G.Thickness          = bFoliage ? (float)((Flags >> GBUFFER_THICKNESS_SHIFT) & GBUFFER_THICKNESS_MASK) / 15.0 : 0.0;
    return G;
}

// The extension targets come from the scene globals, so every pass reading the GBuffer gets them without new arguments.
FGBufferData LoadGBuffer(FGBufferTargets T, int2 Texel)
{
    FGBufferData G = UnpackGBuffer(T.A.Load(Texel),
                                   T.B.Load(Texel),
                                   T.C.Load(Texel),
                                   T.D.Load(Texel));
    if (G.bExtended && Scene().GBufferE.IsValid() && Scene().GBufferF.IsValid())
    {
        UnpackGBufferExtension(G, Scene().GBufferE.Load(int3(Texel, 0)), Scene().GBufferF.Load(int3(Texel, 0)));
    }
    else
    {
        G.bExtended = false;
    }
    return G;
}
