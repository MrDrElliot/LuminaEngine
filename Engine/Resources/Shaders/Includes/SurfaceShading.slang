#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/MeshletGeometry.slang"
#include "Includes/IBLRuntime.slang"
#include "Includes/ShadowSampling.slang"
#include "Includes/ParallaxOcclusion.slang"
#include "Includes/DistanceField.slang"

// Shared opaque/translucent surface shading, factored out of the geometry pixel shaders so the FORWARD
// base pass (BasePixelPass.slang, rasterizer-interpolated FBaseVertexOutput) and the DEFERRED VisBuffer
// material pass (DeferredMaterial.slang, barycentric-reconstructed FBaseVertexOutput) run the exact same
// lighting and can never drift. Everything here is independent of how the FBaseVertexOutput was produced.
//
// DBuffer decals are an opaque-only input bound at set 2; the translucent variant uses set 2 as an empty
// placeholder, so the bindings only exist when not building TRANSLUCENT.
#ifndef TRANSLUCENT
#include "Includes/DBuffer.slang"
#endif

// Compile-time-uniform feature gates, specialized per-pipeline at creation (FForwardRenderScene::
// GetOrCreatePipeline feeds these as constant ids 1-3; the cache keys on them). They default to ENABLED so
// offline slangc validation and any unspecialized pipeline keep full-feature behavior bit-for-bit. When a
// gate is specialized to 0 the SPIR-V optimizer dead-strips the guarded block (and its resource access)
// entirely -- this is how one shared shading path serves passes that bind different resources:
//   DECALS off  -> the terrain pass (which binds FTerrainPushConstants, NOT the DBuffer overlay) never reads
//                  the decal push, so its pointer-bearing push range is not misread as bindless indices;
//   SSAO   off  -> terrain skips the screen-space AO fetch it has no input for (ambient stays unoccluded);
//   DEBUG  off  -> game/shipping pipelines strip the editor view-mode switch (no runtime branch, smaller PS).
[vk::constant_id(1)] const uint SPEC_ENABLE_DEBUG_VIEWS = 1;
[vk::constant_id(2)] const uint SPEC_ENABLE_DECALS      = 1;
[vk::constant_id(3)] const uint SPEC_ENABLE_SSAO        = 1;

// Deferred sun shadows: when 1, the directional branch of ComputeShadowFactor returns a screen-space
// mask ShadowMaskPixel.slang already resolved, and the whole cascade PCSS (two inlined
// SampleDirectionalCascade bodies, each holding a 4x4 cascade VP, the receiver-plane Jacobian, an atlas
// tile and a rolling tap loop) is dead-stripped from this shader.
//
// This gate exists for REGISTER PRESSURE, not for cost: whole-shader VGPR allocation takes the peak
// across every path, so a pixel that never touches the sun still paid the cascade PCSS's live range.
// The mask pass runs the identical code over depth alone, where it is a small shader that reaches high
// occupancy and can actually hide its own texture latency.
//
// Defaults to 0 so unspecialized pipelines, offline slangc validation and any view that does not run
// the pass keep the inline path bit-for-bit. Opt in per-pass via SF_ShadowMask; the pass and the
// pipeline key are driven by the same per-view flag, so a specialized pipeline always has a live mask.
// TRANSLUCENT never opts in -- the mask is resolved from OPAQUE depth, so at a glass pixel it holds the
// shadowing of whatever is BEHIND the glass.
[vk::constant_id(6)] const uint SPEC_SHADOW_MASK        = 0;

// Hash-to-color for the meshlet debug view; stable per-meshlet hues across frames.
float3 MeshletDebugColor(uint Index)
{
    uint H = Index * 2654435761u;
    H ^= H >> 16;
    H *= 2246822519u;
    H ^= H >> 13;
    H *= 3266489917u;
    H ^= H >> 16;

    return float3(
        float( H         & 0xFFu) / 255.0,
        float((H >>  8)  & 0xFFu) / 255.0,
        float((H >> 16)  & 0xFFu) / 255.0
    );
}

struct FCubemapCoord
{
    int Face;
    float2 UV;
};

float3 FresnelSchlick(float CosTheta, float3 F0)
{
    float x = saturate(1.0 - CosTheta);
    float x2 = x * x;
    return F0 + (1.0 - F0) * (x2 * x2 * x);
}

// Half-precision Schlick Fresnel for the light loop. CosTheta and F0 are both [0,1], so fp16 is ample
// and the (1-cos)^5 chain stays well within range.
half3 FresnelSchlickHalf(half CosTheta, half3 F0)
{
    half x  = saturate(half(1.0) - CosTheta);
    half x2 = x * x;
    return F0 + (half3(1.0) - F0) * (x2 * x2 * x);
}

// Vertex-tangent TBN: MikkTSpace import tangent, Gram-Schmidt'd against the interpolated normal here;
// TangentWS.w preserves the bitangent across UV mirrors. (A ddx/ddy TBN faceted normal maps per triangle.)
// The frame itself lives in ParallaxOcclusion.slang so the POM marcher offsets UVs along the exact basis
// the normal map is decoded through -- two copies would silently shear parallax against lighting.
float3 GetWorldNormal(float3 FragNormal, float4 FragTangent, float3 TangentSpaceNormal)
{
    bool bDegenerate;
    float3x3 TBN = BuildTangentFrame(FragNormal, FragTangent, bDegenerate);
    if (bDegenerate)
    {
        // No usable tangent: keep the geometric normal rather than rotating the map into an arbitrary
        // frame, whose axis flips across the |N.z| ~ 1 pole and would seam the surface.
        return FragNormal;
    }
    return normalize(mul(TangentSpaceNormal, TBN));
}

FCubemapCoord DirectionToCubemapCoord(float3 Dir)
{
    FCubemapCoord Result;

    float3 AbsDir = abs(Dir);

    float IsXMax = step(AbsDir.y, AbsDir.x) * step(AbsDir.z, AbsDir.x);
    float IsYMax = step(AbsDir.x, AbsDir.y) * step(AbsDir.z, AbsDir.y);
    float IsZMax = 1.0 - IsXMax - IsYMax;

    float3 FaceIndex = float3(
        lerp(1.0, 0.0, step(0.0, Dir.x)),
        lerp(3.0, 2.0, step(0.0, Dir.y)),
        lerp(5.0, 4.0, step(0.0, Dir.z))
    );

    Result.Face = int(IsXMax * FaceIndex.x + IsYMax * FaceIndex.y + IsZMax * FaceIndex.z);

    float2 UVX = float2( sign(Dir.x) * Dir.z,  Dir.y)                / AbsDir.x;
    float2 UVY = float2(-Dir.x,                -sign(Dir.y) * Dir.z) / AbsDir.y;
    float2 UVZ = float2(-sign(Dir.z) * Dir.x,   Dir.y)                / AbsDir.z;

    Result.UV = IsXMax * UVX + IsYMax * UVY + IsZMax * UVZ;
    Result.UV = Result.UV * 0.5 + 0.5;

    return Result;
}

float ComputeShadowFactor(FLight Light, float3 FragmentPos, float3 Normal, float3 ViewPosition, float2 ScreenPos, bool ReceiveShadow)
{
    if (!ReceiveShadow)
    {
        return 1.0;
    }

    bool IsSpotLight        = HasFlag(Light.Flags, ELightFlags::Spot);
    bool IsDirectionalLight = HasFlag(Light.Flags, ELightFlags::Directional);

    if (IsDirectionalLight)
    {
        // Deferred: ShadowMaskPixel.slang already ran this exact cascade PCSS over the opaque depth
        // buffer, so read its result and let the optimizer strip the whole path from this shader.
        //
        // A second SHADOWED directional light in the cluster list also lands here and gets the sun's
        // mask. That is not a regression: FLightData carries ONE cascade set (CascadeSplits/Radii/
        // Resolutions are singular), so a second directional light already shared the sun's cascades
        // and computed a near-identical factor on the inline path.
        if (SPEC_SHADOW_MASK != 0)
        {
            return SampleShadowMask(ScreenPos);
        }
        return ComputeDirectionalShadow(Light, FragmentPos, Normal, ViewPosition, ScreenPos);
    }

    // Spot / point lights (atlas-based). Atlas stores rasterized NDC z from the shadow VS, so sample
    // by projecting through the same face VP and using ProjCoords.xy/z, not cubemap UV.
    float3 LightDirToFrag = normalize(Light.Position - FragmentPos);

    // Pick the cube face for point lights; spot lights always use face 0.
    int Face = 0;
    if (!IsSpotLight)
    {
        // normalize(FragmentPos - Light.Position) == -LightDirToFrag (negation commutes through
        // normalize), so reuse it instead of a second normalize.
        Face = DirectionToCubemapCoord(-LightDirToFrag).Face;
    }

    // Normal-offset the receiver before projecting; without it walls self-shadow at grazing angles
    // inside the face cone, leaving a dimmer square centered on the light.
    FLightShadow ShadowTile = GetShadowTile(Light.ShadowDataIndex, Face);

    uint AtlasW, AtlasH;
    ShadowAtlas().GetDimensions(AtlasW, AtlasH);

    float  AtlasTileTexels  = max(ShadowTile.AtlasUVScale.x, 1e-6) * float(AtlasW);
    float  WorldTexel       = (Light.Radius * 2.0) / max(AtlasTileTexels, 1.0);
    float  NdotL            = saturate(dot(Normal, LightDirToFrag));
    float  OffsetScale      = saturate(1.0 - NdotL);
    float3 OffsetPos        = FragmentPos + Normal * (WorldTexel * 1.4 * OffsetScale);

    // Receiver bias: constant + slope + tile-quantization term (NDC z quantizes to ~2/AtlasTileTexels),
    // so a fixed bias tuned for 4096 texels self-shadows at 512 and below.
    float SlopeFactor = sqrt(max(1.0 - NdotL * NdotL, 0.0));
    float Bias        = 0.00005 + 0.0003 * SlopeFactor + 1.5 / max(AtlasTileTexels, 1.0);

    float4x4 ShadowVP   = GetShadowViewProjection(Light.ShadowDataIndex, Face);
    float4 ShadowCoord  = mul(ShadowVP, float4(OffsetPos, 1.0));
    float3 ProjCoords   = ShadowCoord.xyz / max(ShadowCoord.w, 1e-6);
    float2 ProjectionUV = ProjCoords.xy * 0.5 + 0.5;

    // Only early-out on z (past the far plane). A UV early-out on [0,1] left a visible square at the
    // face-cone boundary; the AM_Clamp atlas sampler already handles taps straying beyond the tile.
    if (ProjCoords.z < 0.0 || ProjCoords.z > 1.0)
        return 1.0;

    float2 SafeUV = saturate(ProjectionUV);

    float IGN   = frac(52.9829189 * frac(dot(ScreenPos, float2(0.06711056, 0.00583715))));
    float Angle = IGN * TWO_PI;
    // Filter radius (atlas UV) capped at 30% of the tile extent so the Vogel disk stays in the face rect.
    float TileUVExtent  = min(ShadowTile.AtlasUVScale.x, ShadowTile.AtlasUVScale.y);
    float FilterRadius  = min(0.002, TileUVExtent * 0.3);

    float RefDepth = ProjCoords.z - Bias;

    // Build each tap's UV directly in atlas space: SampleUV = BaseUV + Offset. The old form divided the
    // offset by AtlasUVScale to bounds-check in tile-local [0,1], then multiplied straight back -- that
    // round-trip cancels, so the per-tap divide vanishes and only the tile-rect test remains.
    float2 BaseUV  = ShadowTile.AtlasUVOffset + SafeUV * ShadowTile.AtlasUVScale;
    float2 TileMin = ShadowTile.AtlasUVOffset;
    float2 TileMax = ShadowTile.AtlasUVOffset + ShadowTile.AtlasUVScale;

    float Shadow = 0.0;
    float Weight = 0.0;
    // [loop] (not unrolled): SHADOW_SAMPLE_COUNT is a compile-time constant, so an unrolled body holds all
    // 8 tap temporary-sets live at once and inflates the PS register allocation for the WHOLE shader (the
    // allocator reserves the peak across every path, so even sun-only pixels pay it) -> warp-launch stall.
    // A rolling loop keeps one tap's temps; extra resident warps re-hide the SampleCmp latency. Matches the
    // directional PCSS loops above.
    [loop]
    for (int i = 0; i < SHADOW_SAMPLE_COUNT; i++)
    {
        float2 Offset   = VogelDiskSample(i, SHADOW_SAMPLE_COUNT, Angle) * FilterRadius;
        float2 SampleUV = BaseUV + Offset;
        // Skip taps that leave the face rect (those texels belong to another light's tile);
        // the clamped center tap below guarantees at least one valid sample.
        if (any(SampleUV < TileMin) || any(SampleUV > TileMax))
        {
            continue;
        }
        Shadow += ShadowAtlas().SampleCmpLevelZero(SampleUV, RefDepth);
        Weight += 1.0;
    }

    if (Weight <= 0.0)
    {
        return ShadowAtlas().SampleCmpLevelZero(BaseUV, RefDepth);
    }
    return Shadow / Weight;
}

// Walk the light table once to find the sun's shadow-data slot. Used by the
// cascade debug visualizer; pixel-shader cost is irrelevant in that mode.
int FindSunShadowDataIndex()
{
    if (LightData().bHasSun == 0u)
    {
        return INDEX_NONE;
    }
    uint Count = GetNumLights();
    for (uint i = 0; i < Count; ++i)
    {
        FLight L = GetLightAt(i);
        if (HasFlag(L.Flags, ELightFlags::Directional) && L.ShadowDataIndex != INDEX_NONE)
        {
            return L.ShadowDataIndex;
        }
    }
    return INDEX_NONE;
}

// Per-cascade tint for the Shadow Cascades view, mirroring ComputeShadowFactor's selection.
// Dark gray = no sun shadow data; muted tint = fragment outside the selected cascade's bounds.
float3 CascadeDebugColor(float3 FragmentPos, float3 ViewPosition, float3 ShadedBase)
{
    int SunShadowIdx = FindSunShadowDataIndex();
    if (SunShadowIdx == INDEX_NONE)
    {
        return float3(0.08, 0.08, 0.08);
    }

    int CascadeIndex = 0;
    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        if (ViewPosition.z > LightData().CascadeSplits[i])
            CascadeIndex = i + 1;
    }
    if (CascadeIndex >= NUM_CASCADES)
    {
        return float3(0.18, 0.18, 0.18);
    }

    float4x4 CascadeVP  = GetShadowViewProjection(SunShadowIdx, CascadeIndex);
    float4   ShadowCoord = mul(CascadeVP, float4(FragmentPos, 1.0));
    float3   ProjCoords  = ShadowCoord.xyz / max(ShadowCoord.w, 1e-6);
    float2   ProjUV      = ProjCoords.xy * 0.5 + 0.5;
    bool     Inside      = !(any(ProjUV < 0.0) || any(ProjUV > 1.0) ||
                              ProjCoords.z < 0.0 || ProjCoords.z > 1.0);

    const float3 CascadeTints[3] = {
        float3(1.00, 0.25, 0.25),
        float3(0.25, 1.00, 0.30),
        float3(0.30, 0.45, 1.00),
    };
    float3 Tint = CascadeTints[CascadeIndex];

    // Blend with a luminance proxy from the lit shading so silhouettes stay
    // readable while the cascade color dominates.
    float Luma = saturate(Luminance(ShadedBase));
    float3 Color = lerp(Tint * 0.4, Tint, 0.5 + 0.5 * Luma);

    if (!Inside)
    {
        Color *= 0.35;
    }
    return Color;
}

// Penumbra diagnostic for the "Shadow Penumbra" view. Re-runs the cascade pick + PCSS blocker search
// (matching SampleDirectionalCascade) and paints the filter behavior directly:
//   R = FilterRadius / uncapped MaxRadius   (1.0 -> penumbra is pinned at the per-cascade cap)
//   G = FilterRadius in shadow texels / 32  (absolute world-texel width -> what makes it blocky)
//   B = NumBlockers / blocker taps          (blocker-search coverage / stability)
// Black = fully lit (no blockers found); dark gray = no sun or outside every cascade.
float3 ShadowPenumbraDebug(float3 FragmentPos, float3 ViewPosition, float2 ScreenPos)
{
    int SunShadowIdx = FindSunShadowDataIndex();
    if (SunShadowIdx == INDEX_NONE)
        return float3(0.08, 0.08, 0.08);

    int CascadeIndex = 0;
    for (int i = 0; i < NUM_CASCADES; ++i)
        if (ViewPosition.z > LightData().CascadeSplits[i])
            CascadeIndex = i + 1;
    if (CascadeIndex >= NUM_CASCADES)
        return float3(0.15, 0.15, 0.15);

    float4x4 CascadeVP  = GetShadowViewProjection(SunShadowIdx, CascadeIndex);
    float4 ShadowCoord  = mul(CascadeVP, float4(FragmentPos, 1.0));
    float3 ProjCoords   = ShadowCoord.xyz / max(ShadowCoord.w, 1e-6);
    float2 ProjectionUV = ProjCoords.xy * 0.5 + 0.5;
    float  CurrentDepth = ProjCoords.z;
    if (any(ProjectionUV < 0.0) || any(ProjectionUV > 1.0) || CurrentDepth < 0.0 || CurrentDepth > 1.0)
        return float3(0.15, 0.15, 0.15);

    float CascadeScale = 1.0 + 3.0 * float(CascadeIndex) / float(max(NUM_CASCADES - 1, 1));
    float SearchRadius = PCSS_BlockerSearchRadius * CascadeScale;

    float2 dUVdx = ddx(ProjectionUV);
    float2 dUVdy = ddy(ProjectionUV);
    float  dDdx  = ddx(CurrentDepth);
    float  dDdy  = ddy(CurrentDepth);
    float  Det   = dUVdx.x * dUVdy.y - dUVdx.y * dUVdy.x;
    float2 DepthSlopeUV = float2(0.0, 0.0);
    if (abs(Det) > 1e-12)
    {
        float InvDet = 1.0 / Det;
        DepthSlopeUV.x = ( dUVdy.y * dDdx - dUVdx.y * dDdy) * InvDet;
        DepthSlopeUV.y = (-dUVdy.x * dDdx + dUVdx.x * dDdy) * InvDet;
    }

    float BlockerDepthSum = 0.0;
    int   NumBlockers     = 0;
    // [loop] (not [unroll]): see the directional blocker search -- rolling collapses 8 live tap-temp sets to one.
    [loop]
    for (int s = 0; s < PCSS_BLOCKER_SEARCH_TAPS; ++s)
    {
        float2 Offset    = VogelDiskSample(s, PCSS_BLOCKER_SEARCH_TAPS, 0.0) * SearchRadius;
        float2 SampleUV  = clamp(ProjectionUV + Offset, 0.001, 0.999);
        float2 AtlasUV   = CascadeAtlasUV(SunShadowIdx, CascadeIndex, SampleUV);
        float  Depth     = ShadowCascade().SampleLevel(AtlasUV, 0.0).r;
        float  PlaneDelta = clamp(dot(DepthSlopeUV, Offset), -PCSS_MaxPlaneDepthBias, PCSS_MaxPlaneDepthBias);
        if (Depth < CurrentDepth + PlaneDelta - PCSS_BlockerDepthBias)
        {
            BlockerDepthSum += Depth;
            NumBlockers     += 1;
        }
    }

    if (NumBlockers == 0)
        return float3(0.0, 0.0, 0.0);

    float AvgBlockerDepth = BlockerDepthSum / float(NumBlockers);
    float CascadeTexelUV  = 1.0 / max(LightData().CascadeResolutions[CascadeIndex], 1.0);
    float MinRadius       = PCSS_MinFilterTexels * CascadeTexelUV;
    float MaxRadius       = PCSS_MaxFilterTexels * CascadeTexelUV;
    float DepthDelta      = max(CurrentDepth - AvgBlockerDepth, 0.0);
    float Penumbra        = DepthDelta * LightData().ShadowParams.z;
    float FilterRadius    = clamp(Penumbra, MinRadius, MaxRadius);
    float BlockerCoverage = float(NumBlockers) / float(PCSS_BLOCKER_SEARCH_TAPS);
    FilterRadius          = max(FilterRadius * lerp(0.75, 1.0, BlockerCoverage), MinRadius);

    float FilterTexels = FilterRadius / CascadeTexelUV;
    return float3(saturate(FilterRadius / max(MaxRadius, 1e-6)),
                  saturate(FilterTexels / 32.0),
                  BlockerCoverage);
}

// Per-pixel surface terms that don't change across the light loop. Precomputed once so the per-light
// BRDF only re-derives the L-dependent half (NdotL, the light geometry term, H, Fresnel). The [0,1]-range
// BRDF weights are stored half: they tolerate fp16 and halve register pressure in the loop. NdotV (a
// specular denominator) and A2 (= roughness^4, underflows fp16 at the floor) stay float.
struct FShadingContext
{
    float3 N;
    float3 V;
    float  NdotV;        // max(dot(N,V), 1e-4)
    float  A2;           // roughness^4 -- kept full precision so low-roughness highlights don't underflow
    half   Kg;           // Smith-GGX k = (roughness+1)^2 / 8
    half   Gv;           // Schlick-GGX view geometry term (loop-invariant half of Smith G)
    half3  F0;
    half3  DiffuseBase;  // Albedo * (1 - Metallic) / PI
    half3  EnergyComp;   // multi-scatter compensation (Turquin 2019)
};

// L: unit surface->light direction (caller supplies it for every light type, including the sun, so the
// directional Light.Direction normalize happens once at the call site). Distance is unused for directional.
// Diffuse and specular come back separately so the translucent path can keep reflections
// un-premultiplied; opaque callers use the combined wrapper below (identical codegen once inlined).
void EvaluateLightContributionSplit(FLight Light, FShadingContext S, float3 L, float Distance,
                                    out float3 OutDiffuse, out float3 OutSpecular)
{
    OutDiffuse  = float3(0.0);
    OutSpecular = float3(0.0);

    float NdotLf = max(dot(S.N, L), 0.0);
    if (NdotLf <= 0.0)
    {
        return;
    }

    float Attenuation = 1.0;
    float Falloff     = 1.0;

    if (!HasFlag(Light.Flags, ELightFlags::Directional))
    {
        Attenuation     = 1.0 / max(Distance * Distance, 1e-8);
        float DistRatio = Distance / Light.Radius;
        Attenuation    *= 1.0 - smoothstep(clamp(Light.Falloff, 0.0, 1.0), 1.0, DistRatio);

        if (HasFlag(Light.Flags, ELightFlags::Spot))
        {
            float3 LightDir = normalize(Light.Direction.xyz);
            float  CosTheta = dot(LightDir, L);
            Falloff = clamp((CosTheta - Light.Angles.y) / max(Light.Angles.x - Light.Angles.y, 0.001), 0.0, 1.0);
        }
    }

    float3 Radiance = GetLightColor(Light).rgb * Light.Intensity * Attenuation * Falloff;
    float3 H        = normalize(S.V + L);

    // NDF stays full precision: A2 is float and NdotH near 1.0 needs the mantissa.
    float NdotH = max(dot(S.N, H), 0.0);
    float Denom = NdotH * NdotH * (S.A2 - 1.0) + 1.0;
    float NDF   = S.A2 / (PI * Denom * Denom);

    // [0,1]-range BRDF weights in fp16: light geometry term, Fresnel, diffuse/spec tint.
    half  NdotL = (half)NdotLf;
    half  HdotV = (half)max(dot(H, S.V), 0.0);
    half  Gl    = NdotL / (NdotL * (half(1.0) - S.Kg) + S.Kg);
    half  G     = S.Gv * Gl;
    half3 F     = FresnelSchlickHalf(HdotV, S.F0);

    half3 SpecTint = G * F * S.EnergyComp;            // directional/color part of the specular lobe
    half3 Diffuse  = (half3(1.0) - F) * S.DiffuseBase;

    // Recombine in float: NDF and the 1/(4 NdotV NdotL) term carry the HDR dynamic range.
    float  SpecScale = NDF / (4.0 * S.NdotV * NdotLf);
    float3 Spec      = float3(SpecTint) * SpecScale;

    OutDiffuse  = float3(Diffuse) * Radiance * NdotLf;
    OutSpecular = Spec * Radiance * NdotLf;
}

float3 EvaluateLightContribution(FLight Light, FShadingContext S, float3 L, float Distance)
{
    float3 Diffuse, Specular;
    EvaluateLightContributionSplit(Light, S, L, Distance, Diffuse, Specular);
    return Diffuse + Specular;
}

// Result of shading a surface: the lit HDR color plus the material alpha. Callers pack their own
// render-target output (opaque -> Color/Picker, translucent -> weighted Accum/Revealage).
struct FShadedSurface
{
    float3 Color;       // opaque: full lit color. Translucent: transmission-modulated terms only (diffuse + ambient diffuse + emissive)
    float3 Reflection;  // translucent only: specular from lights + IBL, kept out of Color so it isn't attenuated by alpha. Zero for opaque.
    float  Alpha;
    // Translucent only: directional-hemisphere specular reflectance (split-sum single-scatter albedo),
    // i.e. the fraction of incident light this surface reflects instead of transmitting. The WBOIT
    // packing adds it to coverage: reflected energy never reaches what's behind, so stacked glass
    // darkens the background like real glass instead of summing reflections toward white. Zero for opaque.
    float  ReflectionCoverage;
};

// The complete opaque/translucent lighting sequence, given a fully populated FBaseVertexOutput (however it
// was produced -- rasterizer-interpolated in the forward base pass, or barycentric-reconstructed in the
// deferred VisBuffer pass) and the material graph's already-evaluated FMaterialPixelInputs. Masked materials
// `discard` here, so callers must be pixel shaders. UNLIT / TRANSLUCENT follow the same #defines as before.
FShadedSurface ShadeSurface(FBaseVertexOutput Input, FMaterialPixelInputs Material)
{
    uint EntityID           = Input.EntityID;
    uint MaterialIndex      = Input.MaterialIndex;
    bool bReceiveShadow     = (Input.InstanceFlags & (uint)EInstanceFlags::ReceiveShadow) != 0u;
    float3 WorldNormal      = normalize(Input.NormalWS.xyz);
    float3 ViewPosition     = Input.FragPos.xyz;
    float3 WorldPosition    = mul(GetInverseCameraView(), float4(Input.FragPos.xyz, 1.0)).xyz;
    float2 UV0              = Input.UV;
    float4 VertexColor      = Input.FragColor;

    float3 Albedo    = Material.Diffuse;
    float3 Normal    = Material.Normal;
    float  Alpha     = Material.Opacity;
    float  AO        = Material.AmbientOcclusion;
    float  Roughness = max(Material.Roughness, 0.045);
    float  Metallic  = Material.Metallic;

    uint MatFlags = Materials()[MaterialIndex].Flags;
    if (HasFlag(MatFlags, MatFlag_Masked))
    {
        float ClipValue = Materials()[MaterialIndex].OpacityClipValue;
        if (Alpha < ClipValue)
        {
            discard;
        }
    }

#ifdef UNLIT

    float3 Color    = float3(Albedo + Material.Emissive);

#else

    float3 N  = GetWorldNormal(WorldNormal, Input.TangentWS, Normal);
    // Under ortho every ray is parallel, so the eye vector is constant rather than radiating from
    // the camera position; using the position would skew specular toward the viewport center.
    float3 V  = IsCameraOrthographic() ? -GetCameraForward() : normalize(GetCameraPosition() - WorldPosition);

    // DBuffer decals: composite projected decal attributes into the surface BEFORE lighting so they
    // receive lights/shadows/IBL. No-op on pixels no decal touched. Opaque base pass only, and only on
    // pipelines that actually bind the decal push (SPEC_ENABLE_DECALS) -- terrain specializes this off.
#ifndef TRANSLUCENT
    if (SPEC_ENABLE_DECALS != 0)
    {
        float2 DBufferUV = Input.Position.xy / float2(Scene().ScreenSize.xy);
        ApplyDBuffer(Albedo, N, Roughness, Metallic, AO, DBufferUV);
    }
#endif

    // Dielectric F0 = 0.08 * Specular (UE4 convention; default 0.5 -> 0.04). Metals override F0 with albedo.
    float3 F0_DIELECTRIC = float3(0.08 * Material.Specular);
    float3 F0 = lerp(F0_DIELECTRIC, Albedo, Metallic);

    // Multi-scatter energy compensation factor (Turquin 2019), constant per pixel; tinted by F0
    // so colored metals get colored multi-bounce. Ess = single-scatter albedo (BRDF LUT scale+bias).
    float  NdotV_EC    = saturate(dot(N, V));
    float2 DFG_EC      = SampleBRDFLUT(NdotV_EC, Roughness);

#ifdef TRANSLUCENT
    // See FShadedSurface::ReflectionCoverage. Reuses the already-fetched BRDF LUT terms; the
    // FresnelSchlickRoughness call CSEs with the identical one inside EvaluateIBLAmbientSplit.
    float3 FssEssCov          = FresnelSchlickRoughness(NdotV_EC, F0, Roughness) * DFG_EC.x + DFG_EC.y;
    float  ReflectionCoverage = saturate(Luminance(FssEssCov));
#endif
    // EnergyComp is computed below, just before its only use, so this BRDF-LUT fetch's latency overlaps the
    // independent context ALU instead of stalling here.

    // Loop-invariant surface terms: hoisted out of the per-light BRDF so each light only re-derives the
    // L-dependent half. NdotV / roughness remap / view geometry term are identical across every light.
    FShadingContext Ctx;
    Ctx.N           = N;
    Ctx.V           = V;
    Ctx.NdotV       = max(NdotV_EC, 1e-4);
    float RoughSq   = Roughness * Roughness;
    Ctx.A2          = RoughSq * RoughSq;
    float Kr        = Roughness + 1.0;
    float KgF       = (Kr * Kr) * 0.125;
    Ctx.Kg          = (half)KgF;
    Ctx.Gv          = (half)(Ctx.NdotV / (Ctx.NdotV * (1.0 - KgF) + KgF));
    Ctx.F0          = (half3)F0;
    Ctx.DiffuseBase = (half3)(Albedo * (1.0 - Metallic) * INV_PI);
    float3 EnergyComp = 1.0 + F0 * (1.0 / max(DFG_EC.x + DFG_EC.y, 1e-4) - 1.0);
    Ctx.EnergyComp  = (half3)EnergyComp;

    // Shared froxel-index math (SceneGlobals). It clamps to ZNear: edge fragments below it make log()
    // negative, wrapping ZTile to a huge value that overflows Clusters() -> garbage light list and an
    // MMU fault.
    uint TileIndex  = ComputeClusterIndex(ViewPosition.z, Input.Position.xy);
    uint LightCount = Clusters()[TileIndex].Count;

    float3 Lo = float3(0.0);
#ifdef TRANSLUCENT
    // Reflected specular is accumulated apart from transmission-modulated terms; the extra live
    // accumulator is translucent-only so the opaque loop's register footprint is untouched.
    float3 LoSpec = float3(0.0);
#endif

    if (LightData().bHasSun != 0)
    {
        FLight Light = GetLightAt(0);

        // Pixels facing away from the sun are fully self-shadowed (NdotL clamp), so skip the
        // expensive cascade PCSS for them (~half a typical scene). SunL is reused by the BRDF below.
        float3 SunL    = normalize(Light.Direction.xyz);
        float  SunNdotL = dot(N, SunL);
        if (SunNdotL > 0.0)
        {
            // Cascade shadow, then the material's own height-field self-occlusion. Applied here and not
            // to the whole light loop because the parallax trace only ever ran against the sun.
            float Shadow = ComputeShadowFactor(Light, WorldPosition, N, ViewPosition, Input.Position.xy, bReceiveShadow);
            Shadow *= Material.SelfShadow;
#ifdef TRANSLUCENT
            float3 SunDiffuse, SunSpec;
            EvaluateLightContributionSplit(Light, Ctx, SunL, 0.0, SunDiffuse, SunSpec);
            Lo     += SunDiffuse * Shadow;
            LoSpec += SunSpec * Shadow;
#else
            Lo += EvaluateLightContribution(Light, Ctx, SunL, 0.0) * Shadow;
#endif
        }
    }

    // Hard upper bound mirrors LightCull's write cap; protects against any path
    // (e.g. uninitialised cluster memory) that leaves Count outside [0, LIGHTS_PER_CLUSTER].
    uint SafeLightCount = min(LightCount, (uint)LIGHTS_PER_CLUSTER);
    uint NumLights      = GetNumLights();   // loop-invariant; hoisted out of the per-light test
    for (uint i = 0; i < SafeLightCount; ++i)
    {
        uint LightIndex = Clusters()[TileIndex].LightIndices[i];
        // Cluster contents can be stale after a scene switch; an index past the live count reads
        // beyond Lights[MAX_LIGHTS] -> MMU fault in GetLightAt. Skip out-of-range indices.
        if (LightIndex >= NumLights)
        {
            continue;
        }

        // Cheap culls read only Position/Radius; the full 64B light is fetched only for survivors, so every
        // distance- or back-face-culled light skips ~48B of global load and never becomes live here.
        float3 LightPos = LightData().Lights[LightIndex].Position;
        float  Radius   = LightData().Lights[LightIndex].Radius;

        float3 LightToFrag = LightPos - WorldPosition;
        float  DistSq      = dot(LightToFrag, LightToFrag);
        if (DistSq >= Radius * Radius)
        {
            continue;
        }

        float  InvDist = rsqrt(max(DistSq, 1e-8));
        float3 L       = LightToFrag * InvDist;

        // Cluster membership is conservative: a listed light may light only this pixel's back face,
        // which contributes zero, so skip the shadow + BRDF work (and the full light fetch) for it.
        if (dot(N, L) <= 0.0)
        {
            continue;
        }

        FLight Light       = GetLightAt(LightIndex);
        float  DistToLight = DistSq * InvDist;

        float Shadow = 1.0;
        if (Light.ShadowDataIndex != INDEX_NONE)
        {
            Shadow = ComputeShadowFactor(Light, WorldPosition, N, ViewPosition, Input.Position.xy, bReceiveShadow);
        }

#ifdef TRANSLUCENT
        float3 LightDiffuse, LightSpec;
        EvaluateLightContributionSplit(Light, Ctx, L, DistToLight, LightDiffuse, LightSpec);
        Lo     += LightDiffuse * Shadow;
        LoSpec += LightSpec * Shadow;
#else
        Lo += EvaluateLightContribution(Light, Ctx, L, DistToLight) * Shadow;
#endif
    }

    // Screen-space AO attenuates the ambient/IBL term only (direct light already has its own occlusion).
    // AOTextureIndex is ~0u for views that didn't run the SSAO pass (e.g. captures) -> treat as unoccluded.
    // Pipelines with no SSAO input (terrain) specialize SPEC_ENABLE_SSAO off so the fetch is dead-stripped.
    // TRANSLUCENT surfaces skip it entirely: the SSAO texture is computed from OPAQUE depth, so at a glass
    // pixel it holds the AO of whatever is BEHIND the glass -- modulating the glass's own ambient and
    // reflections by the backdrop's occlusion is wrong and smears backdrop detail onto the surface.
    float SSAO = 1.0;
#ifndef TRANSLUCENT
    if (SPEC_ENABLE_SSAO != 0 && Scene().SSAOSettings.AOTextureIndex != 0xFFFFFFFFu)
    {
        float2 ScreenUV = Input.Position.xy / float2(Scene().ScreenSize.xy);
        SSAO = SampleTexture2D(Scene().SSAOSettings.AOTextureIndex, SAMPLER_LINEAR_CLAMP, ScreenUV).r;
    }
#endif

    float3 Ambient;
    if (HasIBL())
    {
        // IBL ambient: split-sum convolutions of the sky cube (irradiance by N, GGX-prefiltered specular
        // by R + BRDF LUT). GetAmbientLightIntensity() scales it so the SkyLight Intensity slider applies.
        // Recompute NdotV/DFG locally (N/V/Roughness are already live) so the top-of-shader DFG_EC/NdotV_EC
        // need not stay register-resident across PCSS + the whole clustered-light loop just to feed this branch.
        float  NdotV_IBL  = saturate(dot(N, V));
        float2 DFG_IBL    = SampleBRDFLUT(NdotV_IBL, Roughness);
#ifdef TRANSLUCENT
        float3 IBLDiffuse, IBLSpec;
        EvaluateIBLAmbientSplit(WorldPosition, N, V, NdotV_IBL, DFG_IBL, Albedo, F0, Roughness, Metallic, AO, IBLDiffuse, IBLSpec);
        Ambient = IBLDiffuse * GetAmbientLightIntensity() * SSAO;
        LoSpec += IBLSpec * GetAmbientLightIntensity() * SSAO;
#else
        float3 IBLAmbient = EvaluateIBLAmbient(WorldPosition, N, V, NdotV_IBL, DFG_IBL, Albedo, F0, Roughness, Metallic, AO);
        Ambient = IBLAmbient * GetAmbientLightIntensity() * SSAO;
#endif
    }
    else
    {
        // No environment: a SkyLight alone still lights the scene via a flat diffuse ambient fill
        // (color * intensity from the SkyLight), occluded by material AO and SSAO. Metals carry no diffuse.
        float3 DiffuseColor = Albedo * (1.0 - Metallic);
        Ambient = GetAmbientLightColor() * GetAmbientLightIntensity() * DiffuseColor * AO * SSAO;
    }
    float3 Color = Ambient + Lo + Material.Emissive;

#endif // UNLIT

    // Editor view-mode overrides. SPEC_ENABLE_DEBUG_VIEWS lets game/shipping pipelines strip the whole
    // switch (DebugMode is always NONE there) so there is no per-pixel branch and the PS stays small.
    uint DebugMode = Scene().CullData.DebugMode;
    if (SPEC_ENABLE_DEBUG_VIEWS != 0 && DebugMode != DEBUG_MODE_NONE)
    {
#if defined(TRANSLUCENT) && !defined(UNLIT)
        // Debug views replace Color wholesale; don't composite reflections on top.
        LoSpec = float3(0.0);
#endif
        switch (DebugMode)
        {
            case DEBUG_MODE_UNLIT:
                Color = Albedo + Material.Emissive;
                break;
            case DEBUG_MODE_MESHLETS:
                Color = MeshletDebugColor(Input.MeshletIndex);
                break;
            case DEBUG_MODE_WORLD_NORMAL:
                // Recompute from the interpolant so WorldNormal isn't held live across the lit/PCSS region.
                Color = normalize(Input.NormalWS.xyz) * 0.5 + 0.5;
                break;
            case DEBUG_MODE_SHADING_NORMAL:
            {
            #ifdef UNLIT
                float3 DebugN = GetWorldNormal(WorldNormal, Input.TangentWS, Material.Normal);
            #else
                float3 DebugN = N;
            #endif
                Color = DebugN * 0.5 + 0.5;
                break;
            }
            case DEBUG_MODE_BASE_COLOR:
                Color = Albedo;
                break;
            case DEBUG_MODE_ROUGHNESS:
                Color = float3(Roughness, Roughness, Roughness);
                break;
            case DEBUG_MODE_METALLIC:
                Color = float3(Metallic, Metallic, Metallic);
                break;
            case DEBUG_MODE_AMBIENT_OCCLUSION:
                Color = float3(AO, AO, AO);
                break;
            case DEBUG_MODE_EMISSIVE:
                Color = Material.Emissive;
                break;
            case DEBUG_MODE_UV:
                Color = float3(frac(Input.UV), 0.0);
                break;
            case DEBUG_MODE_LIGHT_COMPLEXITY:
            {
                uint DbgIndex = ComputeClusterIndex(ViewPosition.z, Input.Position.xy);
                Color = LightComplexityColor(Clusters()[DbgIndex].Count);
                break;
            }
            case DEBUG_MODE_CLUSTER_GRID:
            {
                uint DbgIndex = ComputeClusterIndex(ViewPosition.z, Input.Position.xy);
                Color = ClusterDebugColor(DbgIndex, Clusters()[DbgIndex].Count);
                break;
            }
            case DEBUG_MODE_SHADOW_CASCADES:
                Color = CascadeDebugColor(WorldPosition, ViewPosition, Color);
                break;
            case DEBUG_MODE_SHADOW_PENUMBRA:
                Color = ShadowPenumbraDebug(WorldPosition, ViewPosition, Input.Position.xy);
                break;
            case DEBUG_MODE_SSAO:
            {
                // Show the (blurred) screen-space AO factor. White where SSAO is off for this view.
                float DbgAO = 1.0;
                if (Scene().SSAOSettings.AOTextureIndex != 0xFFFFFFFFu)
                {
                    float2 DbgUV = Input.Position.xy / float2(Scene().ScreenSize.xy);
                    DbgAO = SampleTexture2D(Scene().SSAOSettings.AOTextureIndex, SAMPLER_LINEAR_CLAMP, DbgUV).r;
                }
                Color = float3(DbgAO, DbgAO, DbgAO);
                break;
            }
            case DEBUG_MODE_MATERIAL_ID:
                // Stable hue per GPU material slot -- shows which material owns each pixel (the deferred
                // material binning) and works for any surface (mesh or terrain).
                Color = MeshletDebugColor(Input.MaterialIndex);
                break;
            case DEBUG_MODE_PROBE_INFLUENCE:
            {
                // Covered: hue = which probe won, brightness = influence consumed (a gradient toward
                // dark is the BlendDistance cross-fade band).
                // Not covered: a flat diagnostic color naming the reason, because all of these look
                // identical in a lit view (the surface just reflects sky) but have different fixes.
                // Status colors are FULLY SATURATED; covered pixels are always PASTEL (see
                // ProbeDebugColor). Saturation, not hue, is what tells the two apart -- with enough
                // probes some slice will always land near a given hue.
                //   SATURATED RED     - no probes in the scene root at all (extraction or upload)
                //   SATURATED BLUE    - probe cube array unallocated / mip count zero (target creation)
                //   SATURATED YELLOW  - probes exist but none has finished a capture (bake never ran)
                //   SATURATED MAGENTA - baked probes exist, pixel is outside all of them (placement)
                //   ANY PASTEL COLOR  - working; the hue identifies which slice won
                FProbeDebugInfo Dbg = DebugReflectionProbes(WorldPosition);
                switch (Dbg.Status)
                {
                    case kProbeStatusNoProbes:   Color = float3(1.0, 0.0, 0.0); break;
                    case kProbeStatusNoArray:    Color = float3(0.0, 0.3, 1.0); break;
                    case kProbeStatusNoneBaked:  Color = float3(1.0, 0.85, 0.0); break;
                    case kProbeStatusOutsideAll: Color = float3(1.0, 0.0, 1.0); break;
                    default:
                        // ProbeDebugColor, not MeshletDebugColor: the latter's hash maps index 0 to
                        // pure black, which made a single probe on slice 0 look like no coverage at all.
                        // Brightness is floored so a probe that barely reaches this pixel still reads as
                        // its own hue rather than fading toward the no-coverage cases.
                        Color = ProbeDebugColor((uint)Dbg.DominantSlice) * (0.25 + 0.75 * Dbg.TotalWeight);
                        break;
                }
                break;
            }
            case DEBUG_MODE_PROBE_RADIANCE:
            {
                // What the winning probe actually CAPTURED, at mirror roughness and divided back out by
                // coverage so a half-covered pixel still shows the capture at full brightness rather
                // than a dimmed version of it. Sky here means the capture itself contains sky -- the
                // classic cause being a room whose walls are single-sided, which cone/backface culling
                // then lets the capture see straight through.
                FProbeSample DbgSample = SampleReflectionProbes(WorldPosition, reflect(-V, N), 0.0);
                Color = (DbgSample.Weight > 1e-3)
                            ? DbgSample.Radiance / DbgSample.Weight
                            : float3(0.0);
                break;
            }
        }
    }

    FShadedSurface Out;
    Out.Color = Color;
#if defined(TRANSLUCENT) && !defined(UNLIT)
    Out.Reflection         = LoSpec;
    Out.ReflectionCoverage = ReflectionCoverage;
#else
    Out.Reflection         = float3(0.0);
    Out.ReflectionCoverage = 0.0;
#endif
    Out.Alpha = Alpha;
    return Out;
}
