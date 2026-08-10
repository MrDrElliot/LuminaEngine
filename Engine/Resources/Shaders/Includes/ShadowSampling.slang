#pragma once
#include "SceneGlobals.slang"

// The directional/cascade PCSS lives here, not in SurfaceShading.slang, because two consumers must not
// drift: the inline path in ShadeSurface and ShadowMaskPixel.slang's full-screen resolve.

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

// Load, not Sample: the mask is full-res and screen-aligned, so the fetch is an exact 1:1 texel read.
// Guarded on the sentinel so a specialized view whose pass did not run reads fully lit, not texture 0.
float SampleShadowMask(float2 PixelPos)
{
    uint Index = Scene().ShadowMaskIndex;
    if (Index == 0xFFFFFFFFu)
    {
        return 1.0;
    }
    return gTextures2D[Index].Load(int3(int2(PixelPos), 0)).r;
}

// ToLight is the TO-LIGHT vector, matching FLight::Direction. Passing the travel direction instead
// silently inverts NdotL to zero on every lit surface and pins the bias at maximum.
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
// Bounds in SHADOW TEXELS, not UV: a floor so contact shadows still get multi-tap PCF, and a ceiling
// the max tap count covers gap-free. Texel units keep softening consistent across cascades.
static const float PCSS_MinFilterTexels     = 1.5;
static const float PCSS_MaxFilterTexels     = 4.0;     // ~sqrt(SHADOW_MAX_SAMPLE_COUNT) -> coverable at max taps (16)
static const float PCSS_BlockerDepthBias    = 0.0004;  // Bias for blocker classification
static const float PCSS_MaxPlaneDepthBias   = 0.01;    // Cap on the receiver-plane prediction (silhouette-edge guard)

// PIXEL-ONLY: the receiver-plane bias differentiates with ddx/ddy. Guarded rather than left to dead
// code elimination -- an OpDPdx surviving into a compute entry point is invalid SPIR-V.
#ifndef SHADING_COMPUTE

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

    // Outer cascades cover more world per texel, so scale the search and cap to keep the penumbra
    // scene-proportional. Normalized by cascade count so the outermost keeps its softness at 3 or 4.
    float CascadeScale = 1.0 + 3.0 * float(CascadeIndex) / float(max(NUM_CASCADES - 1, 1));

    // Stage 1: blocker search through the point sampler for raw texel depths (the comparison sampler
    // would return a PCF result, useless for classification). No occluder taps -> fully lit, early out.
    float BlockerDepthSum = 0.0;
    int   NumBlockers     = 0;

    float SearchRadius = PCSS_BlockerSearchRadius * CascadeScale;

    // Receiver-plane depth slope in shadow-UV space, from inverting the 2x2 Jacobian. Without it a sloped
    // receiver is misread as an occluder across the far-cascade disk and sprays false penumbra noise.
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

    // Fold the per-tap cascade->atlas transform into loop invariants. clamp commutes through the positive
    // linear atlas transform, so this matches clamping in cascade-UV while dropping the per-tap FMA.
    float2 BaseAtlasUV       = ProjectionUV * Tile.AtlasUVScale + Tile.AtlasUVOffset;
    float2 TileMin           = float2(0.001) * Tile.AtlasUVScale + Tile.AtlasUVOffset;
    float2 TileMax           = float2(0.999) * Tile.AtlasUVScale + Tile.AtlasUVOffset;
    float2 SearchAtlasRadius = SearchRadius * Tile.AtlasUVScale;

    // Fixed rotation for the blocker search: per-pixel rotation of this sparse pattern stipples thin
    // occluders. [loop], not [unroll] -- unrolling holds 8 tap temp-sets live and spikes PS VGPR.
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

    // Bounds in shadow texels: a floor so contact shadows still get multi-tap PCF, and a ceiling the max
    // tap count covers gap-free. Texel units keep softening consistent across cascades.
    float CascadeTexelUV = 1.0 / max(LightData().CascadeResolutions[CascadeIndex], 1.0);
    float MinRadius      = PCSS_MinFilterTexels * CascadeTexelUV;
    float MaxRadius      = PCSS_MaxFilterTexels * CascadeTexelUV;

    // NO early-out on an empty blocker search: it is 8 sparse point taps on a fixed rotation, so a hard
    // 1.0 stamps crisp texel-shaped lit rectangles wherever the pattern misses a real occluder.
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

// Factored out so the sun and any other directional light share the exact same cascade PCSS. Normal
// feeds only the slope bias and normal-offset push, so the mask pass can pass a geometric normal.
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

    // TO-LIGHT, matching the SunL the BRDF uses. Negated into the travel direction, every sun-facing
    // surface scores NdotL = 0, so the offset runs at full strength and pushes samples through casters.
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

#endif // !SHADING_COMPUTE
