#pragma once
#include "SceneGlobals.slang"

// Shadow atlases are reached bindlessly through the scene root (set 3 is gone). The cascade and
// spot/point atlases are 2D SRVs in the bindless table; comparison sampling uses the comparison
// alias of the bindless sampler array, point sampling (PCSS blocker) uses a point-clamp sampler.
//
// The directional/cascade PCSS lives here, not in SurfaceShading.slang, because it now has TWO
// consumers that must not drift: the inline path in ShadeSurface (translucent, capture views) and
// ShadowMaskPixel.slang, the full-screen pass that resolves the sun into a screen-space mask. Both
// call ComputeDirectionalShadow with the same arguments; the only difference is where the normal
// comes from (see the mask pass).

struct FShadowTex
{
    Texture2D              Tex;
    SamplerComparisonState Cmp;
    SamplerState           Pt;

    float  SampleCmp(float2 UV, float Ref)          { return Tex.SampleCmp(Cmp, UV, Ref); }
    float  SampleCmpLevelZero(float2 UV, float Ref) { return Tex.SampleCmpLevelZero(Cmp, UV, Ref); }
    float4 SampleLevel(float2 UV, float Lod)        { return Tex.SampleLevel(Pt, UV, Lod); }
    void   GetDimensions(out uint W, out uint H)    { Tex.GetDimensions(W, H); }
};

FShadowTex MakeShadowTex(uint Index)
{
    FShadowTex S;
    S.Tex = gTextures2D[Index];
    S.Cmp = gCmpSamplers[SAMPLER_SHADOW];
    S.Pt  = gSamplers[SAMPLER_POINT_CLAMP];
    return S;
}

FShadowTex ShadowCascade() { return MakeShadowTex(GetSceneRoot().ShadowCascadeIndex); }
FShadowTex ShadowAtlas()   { return MakeShadowTex(GetSceneRoot().ShadowAtlasIndex); }

// Screen-space sun-shadow mask, written by ShadowMaskPixel.slang over the full opaque depth buffer.
// Load (not Sample): the mask is full-res and screen-aligned, so the fetch is an exact 1:1 texel
// read -- no sampler, no filtering, no UV divide. SV_Position.xy is the pixel center (x.5), which
// truncates to the owning pixel.
//
// Guarded on the sentinel so a view whose pipelines were specialized for the mask but whose pass
// did not run still reads as fully lit rather than sampling texture 0. That should not happen (the
// pipeline key and the pass are driven by the same per-view flag), but silently returning garbage
// would be much harder to spot than an unshadowed scene.
float SampleShadowMask(float2 PixelPos)
{
    uint Index = Scene().ShadowMaskIndex;
    if (Index == 0xFFFFFFFFu)
    {
        return 1.0;
    }
    return gTextures2D[Index].Load(int3(int2(PixelPos), 0)).r;
}

// ToLight is the TO-LIGHT vector (surface -> light), the same convention as FLight::Direction and the
// spot/point branch's normalize(Light.Position - FragmentPos). Passing the travel direction instead
// silently inverts NdotL to zero on every lit surface, which is not a compile error and not obviously
// wrong on open ground: it just pins the bias at maximum. Hence the name rather than "LightDir".
float ComputeShadowBias(uint LightFlags, float3 Normal, float3 ToLight, int CascadeIndex)
{
    float NdotL       = saturate(dot(Normal, ToLight));
    float SlopeFactor = sqrt(max(1.0 - NdotL * NdotL, 0.0));

    if (HasFlag(LightFlags, ELightFlags::Directional))
    {
        // Constant safety bias only; NormalOffsetWS() handles slope acne. ShadowParams.y is the per-light constant bias.
        return 0.00015 + 0.0003 * SlopeFactor + LightData().ShadowParams.y;
    }

    // Point/spot: RefDepth is NDC z from the face VP, which clusters near 1.0
    // and needs a much smaller bias than the old linear DistToLight/Radius.
    return 0.00005 + 0.0003 * SlopeFactor;
}

// Normal-offset bias (Holbert, GDC 2011): push the receiver ~one shadow texel along its normal before
// projecting, scaled by (1 - NdotL) so it only kicks in at grazing angles. Returns the world-space offset.
float3 NormalOffsetWS(float3 Normal, float3 ToLight, int CascadeIndex)
{
    float NdotL       = saturate(dot(Normal, ToLight));
    float OffsetScale = saturate(1.0 - NdotL);

    // World-space size of one shadow texel: ortho width 2*Radius / per-cascade resolution.
    float Radius     = LightData().CascadeRadii[CascadeIndex];
    float Resolution = max(LightData().CascadeResolutions[CascadeIndex], 1.0);
    float TexelSize  = (2.0 * Radius) / Resolution;

    // 1.4 ~= sqrt(2): one diagonal texel at max slope, the minimum that clears depth-quantization
    // self-intersection without peter-panning. ShadowParams.x scales per-light (1.0 = default).
    return Normal * (TexelSize * 1.4 * OffsetScale * LightData().ShadowParams.x);
}

// Pack a per-cascade UV [0,1] into the shared cascade atlas. The atlas tile
// for this cascade is described by FLightShadow.AtlasUVOffset/Scale.
float2 CascadeAtlasUV(int ShadowDataIndex, int CascadeIndex, float2 CascadeUV)
{
    FLightShadow Tile = GetShadowTile(ShadowDataIndex, CascadeIndex);
    return CascadeUV * Tile.AtlasUVScale + Tile.AtlasUVOffset;
}

// PCSS tuning: scale the PCF radius by receiver-to-blocker depth so the kernel collapses to a
// near-point sample at contact and opens to a soft penumbra at distance.
#define PCSS_BLOCKER_SEARCH_TAPS   8
static const float PCSS_BlockerSearchRadius = 0.0035;  // UV radius of blocker search (scaled by CascadeScale)
static const float PCSS_LightSize           = 0.05;     // Tunable: larger = softer far shadows
// Filter radius bounds in SHADOW TEXELS (per-cascade), not UV: a floor so contact shadows still get
// multi-tap PCF (smooth near edges, not blocky), and a ceiling the max tap count covers gap-free (no far
// stipple). Texel units keep softening screen-consistent across cascades without an explicit CascadeScale.
static const float PCSS_MinFilterTexels     = 1.5;
static const float PCSS_MaxFilterTexels     = 4.0;     // ~sqrt(SHADOW_MAX_SAMPLE_COUNT) -> coverable at max taps (16)
static const float PCSS_BlockerDepthBias    = 0.0004;  // Bias for blocker classification
static const float PCSS_MaxPlaneDepthBias   = 0.01;    // Cap on the receiver-plane prediction (silhouette-edge guard)

// PCSS-filtered sample of one directional cascade. Returns 1.0 (and Inside=false) when the fragment
// is outside the cascade's projected range so the caller can fall back to another cascade or fully-lit.
float SampleDirectionalCascade(FLight Light, int CascadeIndex, float3 FragmentPos, float3 Normal, float3 ToLight, float Bias, float2 ScreenPos, out bool Inside)
{
    // Normal-offset the receiver before projecting; removes self-shadow banding depth bias can't fix.
    float3 OffsetPos = FragmentPos + NormalOffsetWS(Normal, ToLight, CascadeIndex);

    float4x4 CascadeVP  = GetShadowViewProjection(Light.ShadowDataIndex, CascadeIndex);
    float4 ShadowCoord  = mul(CascadeVP, float4(OffsetPos, 1.0));
    float3 ProjCoords   = ShadowCoord.xyz / max(ShadowCoord.w, 1e-6);
    float2 ProjectionUV = ProjCoords.xy * 0.5 + 0.5;
    float  CurrentDepth = ProjCoords.z;

    // Screen-space derivatives of the shadow-space depth/UV, taken before the Inside early-out so the
    // quad stays coherent. Used below to predict the receiver's own depth across the blocker search.
    float2 dUVdx = ddx(ProjectionUV);
    float2 dUVdy = ddy(ProjectionUV);
    float  dDdx  = ddx(CurrentDepth);
    float  dDdy  = ddy(CurrentDepth);

    Inside = !(any(ProjectionUV < 0.0) || any(ProjectionUV > 1.0) ||
               CurrentDepth < 0.0 || CurrentDepth > 1.0);
    if (!Inside)
    {
        return 1.0;
    }

    float IGN   = frac(52.9829189 * frac(dot(ScreenPos, float2(0.06711056, 0.00583715))));
    float Angle = IGN * TWO_PI;

    float RefDepth = CurrentDepth - Bias;

    // Outer cascades cover more world per texel, so scale the search and filter cap to keep the
    // penumbra scene-proportional (smaller = sharper but blockier, larger = softer but loses contact).
    // Normalized by cascade count so the outermost cascade keeps the same softness (~4x) whether there
    // are 3 or 4 cascades -- a raw 1+1.5*index would over-blur the far cascade as cascade count grows.
    float CascadeScale = 1.0 + 3.0 * float(CascadeIndex) / float(max(NUM_CASCADES - 1, 1));

    // Stage 1: blocker search through the point sampler for raw texel depths (the comparison sampler
    // would return a PCF result, useless for classification). No occluder taps -> fully lit, early out.
    float BlockerDepthSum = 0.0;
    int   NumBlockers     = 0;

    float SearchRadius = PCSS_BlockerSearchRadius * CascadeScale;

    // Receiver-plane depth slope in shadow-UV space, recovered by inverting the 2x2 [dUV/dscreen] Jacobian.
    // The far-cascade search disk spans many world units, so a sloped receiver (ground at a grazing angle)
    // changes its own depth across it; predicting that per tap stops the surface from being misread as an
    // occluder and spraying false penumbra noise. Degenerate Jacobian -> flat plane (zero slope).
    float  Det = dUVdx.x * dUVdy.y - dUVdx.y * dUVdy.x;
    float2 DepthSlopeUV = float2(0.0, 0.0);
    if (abs(Det) > 1e-12)
    {
        float InvDet = 1.0 / Det;
        DepthSlopeUV.x = ( dUVdy.y * dDdx - dUVdx.y * dDdy) * InvDet;
        DepthSlopeUV.y = (-dUVdy.x * dDdx + dUVdx.x * dDdy) * InvDet;
    }

    // The cascade's atlas tile (UV scale/offset) is identical for every tap; load the 32-byte FLightShadow
    // once here rather than re-fetching it per blocker/PCF tap (SPIR-V LICM doesn't hoist BDA loads).
    FLightShadow Tile = GetShadowTile(Light.ShadowDataIndex, CascadeIndex);

    // Fold the per-tap cascade->atlas transform into loop invariants: AtlasUV = BaseAtlasUV + VogelOffset*Radius,
    // clamped to this tile's bounds. clamp commutes through the positive (per-axis) linear atlas transform, so
    // this is identical to clamping in [0,1] cascade-UV then transforming -- but it removes the per-tap UV add
    // and the scale+offset FMA (the dominant FP32-math cost in both loops below). Radius differs per loop.
    float2 BaseAtlasUV       = ProjectionUV * Tile.AtlasUVScale + Tile.AtlasUVOffset;
    float2 TileMin           = float2(0.001) * Tile.AtlasUVScale + Tile.AtlasUVOffset;
    float2 TileMax           = float2(0.999) * Tile.AtlasUVScale + Tile.AtlasUVOffset;
    float2 SearchAtlasRadius = SearchRadius * Tile.AtlasUVScale;

    // Fixed (non per-pixel) rotation for the blocker search: per-pixel rotation of this sparse pattern
    // stipples thin-occluder penumbras; a shared angle keeps the lit/shadow boundary coherent. PCF below still rotates.
    // [loop] (not [unroll]): unrolling holds 8 tap temporary-sets live at once, spiking PS VGPR -> the
    // dominant warp-launch register stall. A rolling loop keeps one set; extra resident warps re-hide the tap latency.
    [loop]
    for (int s = 0; s < PCSS_BLOCKER_SEARCH_TAPS; ++s)
    {
        float2 VOffset  = VogelDiskSample(s, PCSS_BLOCKER_SEARCH_TAPS, 0.0);
        float2 Offset   = VOffset * SearchRadius;   // cascade-UV space, for the receiver-plane prediction below
        float2 AtlasUV  = clamp(BaseAtlasUV + VOffset * SearchAtlasRadius, TileMin, TileMax);
        float  Depth    = ShadowCascade().SampleLevel(AtlasUV, 0.0).r;

        // Expected receiver depth at this offset along its own plane (clamped so a runaway gradient at a
        // silhouette edge can't invert the test). Only depths in front of the predicted plane are occluders.
        float PlaneDelta    = clamp(dot(DepthSlopeUV, Offset), -PCSS_MaxPlaneDepthBias, PCSS_MaxPlaneDepthBias);
        float ReceiverDepth = CurrentDepth + PlaneDelta;

        if (Depth < ReceiverDepth - PCSS_BlockerDepthBias)
        {
            BlockerDepthSum += Depth;
            NumBlockers     += 1;
        }
    }

    // Filter bounds in shadow texels (per-cascade): a floor so contact shadows still get multi-tap PCF
    // (smooth near edges), and a ceiling the max tap count covers gap-free (no far stipple). Texel units
    // keep softening screen-consistent across cascades, so no explicit CascadeScale is needed here.
    float CascadeTexelUV = 1.0 / max(LightData().CascadeResolutions[CascadeIndex], 1.0);
    float MinRadius      = PCSS_MinFilterTexels * CascadeTexelUV;
    float MaxRadius      = PCSS_MaxFilterTexels * CascadeTexelUV;

    // NO early-out on an empty blocker search. The search is 8 sparse taps of POINT-sampled depth on a FIXED
    // rotation, so whether it finds an occluder is quantized to shadow-map texels; returning a hard 1.0 here
    // skipped the PCF entirely and stamped crisp, fully-lit, texel-shaped rectangles into the penumbra
    // wherever the sparse pattern missed a real occluder. Falling through at MinRadius still resolves those
    // pixels correctly (the PCF compares against the actual shadow map) and costs a few taps where there
    // genuinely is nothing, which returns 1.0 anyway.
    float FilterRadius = MinRadius;
    if (NumBlockers > 0)
    {
        float AvgBlockerDepth = BlockerDepthSum / float(NumBlockers);

        // Stage 2: penumbra grows with receiver-to-blocker depth (ortho NDC-z delta) scaled by per-light
        // softness (ShadowParams.z), clamped between the texel floor and ceiling.
        float DepthDelta = max(CurrentDepth - AvgBlockerDepth, 0.0);
        float Penumbra   = DepthDelta * LightData().ShadowParams.z;
        FilterRadius     = clamp(Penumbra, MinRadius, MaxRadius);

        // Low blocker coverage means partial occlusion, so pull the filter in slightly -- but never below
        // the texel floor, or contact-hardened edges go blocky again.
        float BlockerCoverage = float(NumBlockers) / float(PCSS_BLOCKER_SEARCH_TAPS);
        FilterRadius = max(FilterRadius * lerp(0.75, 1.0, BlockerCoverage), MinRadius);
    }

    // Stage 3: scale tap count with penumbra width so sample density (and thus smoothness) stays roughly
    // constant; at MaxRadius this reaches SHADOW_MAX_SAMPLE_COUNT -> ~1-texel Vogel spacing, gap-free.
    int   BaseTaps    = clamp(int(LightData().ShadowParams2.y), 1, SHADOW_MAX_SAMPLE_COUNT);
    float RadiusFrac  = saturate(FilterRadius / max(MaxRadius, 1e-6));
    int   SampleCount = clamp(int(lerp(float(BaseTaps), float(SHADOW_MAX_SAMPLE_COUNT), RadiusFrac)),
                              BaseTaps, SHADOW_MAX_SAMPLE_COUNT);

    // Same atlas fold as the blocker search. FP16 the unit disk offset (sub-texel, fp16 ~1e-6, packs into one
    // register); the atlas UV + depth-compare ref stay fp32 for addressing precision.
    float2 AtlasFilterRadius = FilterRadius * Tile.AtlasUVScale;

    half Shadow = half(0.0);
    [loop]
    for (int i = 0; i < SampleCount; ++i)
    {
        half2  VOffset = half2(VogelDiskSample(i, SampleCount, Angle + PI));
        float2 AtlasUV = clamp(BaseAtlasUV + float2(VOffset) * AtlasFilterRadius, TileMin, TileMax);
        Shadow        += half(ShadowCascade().SampleCmp(AtlasUV, RefDepth));
    }
    return float(Shadow) * rcp(float(SampleCount));
}

// The directional/cascade (sun) shadow factor. Factored out so the sun (light 0) and any other directional
// light in the clustered loop share the exact same cascade PCSS.
//
// Normal is used for two things only -- the slope term of the depth bias and the normal-offset push -- so
// the screen-space mask pass can feed it a depth-derived GEOMETRIC normal. That is arguably the more
// correct input of the two: normal-offset bias is a fix for shadow-map texel quantization on the RASTERIZED
// surface, which the normal map does not move.
float ComputeDirectionalShadow(FLight Light, float3 FragmentPos, float3 Normal, float3 ViewPosition, float2 ScreenPos)
{
    // Sun with bCastShadows = false leaves ShadowDataIndex at INDEX_NONE;
    // skip the cascade work and treat as fully lit.
    if (Light.ShadowDataIndex == INDEX_NONE)
    {
        return 1.0;
    }

    // Pick current cascade based on view-space Z against the split table (loaded once into registers).
    float4 CascadeSplits = LightData().CascadeSplits;
    int CascadeIndex = 0;
    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        if (ViewPosition.z > CascadeSplits[i])
            CascadeIndex = i + 1;
    }
    if (CascadeIndex >= NUM_CASCADES)
        return 1.0;

    // TO-LIGHT, matching the SunL the BRDF below uses and the spot/point branch's own LightToFrag. This
    // was negated into the light's TRAVEL direction, which inverted the NdotL both bias functions derive
    // from it: every surface facing the sun scored NdotL = 0, so the slope bias sat at its maximum and
    // NormalOffsetWS ran at FULL strength everywhere instead of only at grazing angles. Harmless in the
    // open, where the offset pushes into empty air, but under an overhang it shoved the sample through
    // the caster and the shadow read as lit.
    float3 LightToFrag = normalize(Light.Direction);
    float  Bias        = ComputeShadowBias(Light.Flags, Normal, LightToFrag, CascadeIndex);

    bool  InsideA;
    float ShadowA = SampleDirectionalCascade(Light, CascadeIndex, FragmentPos, Normal, LightToFrag, Bias, ScreenPos, InsideA);

    // Blend into the next cascade near the split boundary so the transition is smooth, not a seam.
    float CascadeNear = (CascadeIndex == 0) ? 0.0 : CascadeSplits[CascadeIndex - 1];
    float CascadeFar  = CascadeSplits[CascadeIndex];
    float CascadeLen  = max(CascadeFar - CascadeNear, 1e-4);
    float LocalT      = saturate((ViewPosition.z - CascadeNear) / CascadeLen);

    // Cross-fade fraction is per-light (ShadowParams.w); BlendStart = 1 - blend.
    const float BlendStart = saturate(1.0 - LightData().ShadowParams.w);
    if (LocalT > BlendStart && CascadeIndex + 1 < NUM_CASCADES)
    {
        float BlendT  = saturate((LocalT - BlendStart) / (1.0 - BlendStart));
        float BiasB   = ComputeShadowBias(Light.Flags, Normal, LightToFrag, CascadeIndex + 1);

        bool  InsideB;
        float ShadowB = SampleDirectionalCascade(Light, CascadeIndex + 1, FragmentPos, Normal, LightToFrag, BiasB, ScreenPos, InsideB);

        if (InsideB)
        {
            return lerp(ShadowA, ShadowB, BlendT);
        }
    }

    // Final cascade: fade to fully lit over the outer slice so shadows
    // dissolve at ShadowMaxDistance instead of popping off.
    if (CascadeIndex + 1 == NUM_CASCADES)
    {
        // Distance-fade fraction is per-light (ShadowParams2.x); start = 1 - fade.
        const float FarFadeStart = saturate(1.0 - LightData().ShadowParams2.x);
        if (LocalT > FarFadeStart)
        {
            float FadeT = saturate((LocalT - FarFadeStart) / (1.0 - FarFadeStart));
            return lerp(ShadowA, 1.0, FadeT);
        }
    }

    return ShadowA;
}
