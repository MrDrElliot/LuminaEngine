#pragma once
#include "SceneGlobals.slang"
#include "IBL.slang"
#include "ReflectionProbe.slang"

// Runtime IBL ambient, Karis 2013 split-sum:
//   Ambient = kD * SkyIrradiance(N) * Albedo + SkyPrefilter(R, roughness) * (F * BRDF_LUT.x + BRDF_LUT.y)

float3 SampleIrradiance(float3 N)
{
    return SampleTextureCubeLevel(GetSceneRoot().SkyIrradianceIndex, SAMPLER_LINEAR_CLAMP, N, 0.0).rgb;
}

// Roughness-driven mip selection. The prefilter cube's mip 0 is mirror,
// last mip is fully rough (matches the bake at PrefilterEnvMapPass).
float3 SamplePrefilter(float3 R, float Roughness)
{
    // SkyPrefilterIndex packs the mip count in its top byte (see FSceneRoot) so the prefilter LOD range is
    // known without a per-pixel GetDimensions on the cube.
    uint Packed         = GetSceneRoot().SkyPrefilterIndex;
    uint PrefilterIndex = Packed & 0x00FFFFFFu;
    uint NumMips        = Packed >> 24;

    float MaxLod = max(float(NumMips) - 1.0, 0.0);
    float Lod    = saturate(Roughness) * MaxLod;
    float3 Prefiltered = SampleTextureCubeLevel(PrefilterIndex, SAMPLER_LINEAR_CLAMP, R, Lod).rgb;

    // The prefilter base is far lower-res than the sky cube, so near-mirror reflections look soft. For low
    // roughness, blend in a direct full-res sky-cube tap (its mip 0 at r=0 is the unconvolved environment,
    // so this is consistent, just sharper). Fades out by r~0.25 where the prefilter blur takes over.
    float SharpBlend = saturate(1.0 - Roughness * 4.0);
    if (SharpBlend > 0.0)
    {
        float3 Sharp = SampleTextureCubeLevel(GetSceneRoot().SkyCubeIndex, SAMPLER_LINEAR_CLAMP, R, 0.0).rgb;
        Prefiltered  = lerp(Prefiltered, Sharp, SharpBlend);
    }
    return Prefiltered;
}

// Specular radiance arriving along R: local probes first, sky for whatever influence they leave.
// The sky prefilter is an infinitely-distant environment, so it is the correct fallback rather than a
// black hole -- a pixel half-covered by one probe reads half local reflection, half sky.
float3 SampleSpecularRadiance(float3 WorldPos, float3 R, float Roughness)
{
    FProbeSample Probes = SampleReflectionProbes(WorldPos, R, Roughness);
    if (Probes.Weight >= 0.999)
    {
        return Probes.Radiance;
    }
    return Probes.Radiance + (1.0 - Probes.Weight) * SamplePrefilter(R, Roughness);
}

float2 SampleBRDFLUT(float NdotV, float Roughness)
{
    return SampleTexture2DLevel(GetSceneRoot().BRDFLutIndex, SAMPLER_LINEAR_CLAMP,
                                 float2(saturate(NdotV), saturate(Roughness)), 0.0).rg;
}

// Frostbite specular occlusion (Lagarde & de Rousiers, "Moving Frostbite to PBR", 2014).
// Widens AO with roughness/grazing angle so the narrower specular cone doesn't leak into cavities.
float ComputeSpecularOcclusion(float NdotV, float AO, float Roughness)
{
    return saturate(pow(NdotV + AO, exp2(-16.0 * Roughness - 1.0)) - 1.0 + AO);
}

// Energy-conserving IBL ambient: Karis split-sum + Fdez-Aguera 2019 multi-scatter compensation.
// N is the shading normal; V points surface->camera. AO occludes diffuse+multi-scatter, SpecAO the mirror.
// NdotV (= saturate(dot(N,V))) and DFG (BRDF LUT scale,bias) are passed in: the caller already sampled the
// LUT for energy compensation, so reusing it here avoids a second per-pixel texture fetch with identical args.
// Split variant: the mirror-lobe specular comes back separately from the irradiance-driven terms so the
// translucent path can keep reflections un-premultiplied (glass attenuates what's behind it, not its own
// reflections). Opaque callers use the combined wrapper below.
void EvaluateIBLAmbientSplit(float3 WorldPos, float3 N, float3 V, float NdotV, float2 DFG, float3 Albedo, float3 F0, float Roughness, float Metallic, float AO,
                             out float3 OutDiffuse, out float3 OutSpecular)
{
    float3 R = reflect(-V, N);

    // Roughness-aware Fresnel as the specular weight (clamps the (1 - cos)^5 envelope to (1 - R)).
    float3 kS     = FresnelSchlickRoughness(NdotV, F0, Roughness);
    float3 FssEss = kS * DFG.x + DFG.y;             // single-scatter specular

    // Multi-scatter compensation (Fdez-Aguera): Ems = energy the single-scatter lobe missed,
    // Favg = cosine-averaged Fresnel; the closed form sums the remaining bounces.
    float  Ems    = saturate(1.0 - (DFG.x + DFG.y));
    float3 Favg   = F0 + (1.0 - F0) / 21.0;
    float3 FmsEms = (Ems * FssEss * Favg) / max(1.0 - Favg * Ems, 1e-4);

    // Metals carry no diffuse; whatever energy isn't reflected as single- or
    // multi-scatter specular goes to diffuse, keeping the surface conserving.
    float3 DiffuseColor = Albedo * (1.0 - Metallic);
    float3 kD           = DiffuseColor * (1.0 - FssEss - FmsEms);

    float3 Irradiance = SampleIrradiance(N);
    // Local probes override the sky here; diffuse irradiance stays sky-only (probes carry no
    // irradiance convolution, so a diffuse local bounce would need a second bake per probe).
    float3 PreFilt    = SampleSpecularRadiance(WorldPos, R, Roughness);

    float SpecAO = ComputeSpecularOcclusion(NdotV, AO, Roughness);

    OutSpecular = FssEss * PreFilt * SpecAO;
    OutDiffuse  = (FmsEms + kD) * Irradiance * AO;
}

float3 EvaluateIBLAmbient(float3 WorldPos, float3 N, float3 V, float NdotV, float2 DFG, float3 Albedo, float3 F0, float Roughness, float Metallic, float AO)
{
    float3 Diffuse, Specular;
    EvaluateIBLAmbientSplit(WorldPos, N, V, NdotV, DFG, Albedo, F0, Roughness, Metallic, AO, Diffuse, Specular);
    return Diffuse + Specular;
}
