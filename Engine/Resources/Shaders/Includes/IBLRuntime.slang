#pragma once
#include "SceneGlobals.slang"
#include "IBL.slang"
#include "ReflectionProbe.slang"

// Karis 2013 split-sum ambient.

float3 SampleIrradiance(float3 N)
{
    return GetSceneRoot().SkyIrradiance.SampleLevel(Samplers::LinearClamp, N, 0.0).rgb;
}

// Prefilter mip 0 is mirror and the last mip fully rough, matching PrefilterEnvMapPass.
float3 SamplePrefilter(float3 R, float Roughness)
{
    const FMippedTextureCube Prefilter = GetSceneRoot().SkyPrefilter;
    uint NumMips = Prefilter.MipCount();

    float MaxLod = max(float(NumMips) - 1.0, 0.0);
    float Lod    = saturate(Roughness) * MaxLod;
    float3 Prefiltered = Prefilter.Texture().SampleLevel(Samplers::LinearClamp, R, Lod).rgb;

    // The prefilter base is low-res, so low roughness blends in a full-res sky-cube tap until r~0.25.
    float SharpBlend = saturate(1.0 - Roughness * 4.0);
    if (SharpBlend > 0.0)
    {
        float3 Sharp = GetSceneRoot().SkyCube.SampleLevel(Samplers::LinearClamp, R, 0.0).rgb;
        Prefiltered  = lerp(Prefiltered, Sharp, SharpBlend);
    }
    return Prefiltered;
}

// A frame with no probes specializes this to 0, dropping the probe walk from the register budget.
[vk::constant_id(12)] const uint SPEC_REFLECTION_PROBES = 1;

float3 SampleSpecularRadiance(float3 WorldPos, float3 R, float Roughness)
{
    if (SPEC_REFLECTION_PROBES == 0)
    {
        return SamplePrefilter(R, Roughness) * GetAmbientLightIntensity();
    }

    // A capture already holds the skylight it was lit with, so only the sky's own share takes the intensity.
    FProbeSample Probes = SampleReflectionProbes(WorldPos, R, Roughness);
    if (Probes.Weight >= 0.999)
    {
        return Probes.Radiance;
    }
    return Probes.Radiance + (1.0 - Probes.Weight) * SamplePrefilter(R, Roughness) * GetAmbientLightIntensity();
}

// A probe's roughest mip stands in for its irradiance, so walls inside a probe stop receiving the open sky.
float3 SampleDiffuseIrradiance(float3 WorldPos, float3 N)
{
    if (SPEC_REFLECTION_PROBES == 0)
    {
        return SampleIrradiance(N) * GetAmbientLightIntensity();
    }

    FProbeSample Probes = SampleReflectionProbes(WorldPos, N, 1.0);
    return Probes.Radiance + (1.0 - Probes.Weight) * SampleIrradiance(N) * GetAmbientLightIntensity();
}

float2 SampleBRDFLUT(float NdotV, float Roughness)
{
    return GetSceneRoot().BRDFLut.SampleLevel(Samplers::LinearClamp, float2(saturate(NdotV), saturate(Roughness)), 0.0).rg;
}

// Specular occlusion from Lagarde and de Rousiers, "Moving Frostbite to PBR" (2014).
float ComputeSpecularOcclusion(float NdotV, float AO, float Roughness)
{
    return saturate(pow(NdotV + AO, exp2(-16.0 * Roughness - 1.0)) - 1.0 + AO);
}

// Oat and Sander 2007, the solid angle two caps share over 2 pi, with the smoothstep falloff Jimenez et al. 2016 suggest.
float SphericalCapIntersection(float CosCap1, float CosCap2, float CosDistance)
{
    const float R1 = acos(CosCap1);
    const float R2 = acos(CosCap2);
    const float D  = acos(clamp(CosDistance, -1.0, 1.0));
    const float SmallerCap = 1.0 - max(CosCap1, CosCap2);
    if (min(R1, R2) <= max(R1, R2) - D)
    {
        return SmallerCap;
    }
    if (R1 + R2 <= D)
    {
        return 0.0;
    }
    const float Delta = abs(R1 - R2);
    return smoothstep(0.0, 1.0, 1.0 - saturate((D - Delta) / max(R1 + R2 - Delta, 1e-4))) * SmallerCap;
}

// Jimenez et al. 2016 cone test of the specular lobe against the open cone around the bent normal, relative to an open hemisphere.
float ComputeBentSpecularOcclusion(float3 BentN, float3 N, float3 R, float NdotV, float AO, float Roughness)
{
    const float Alpha   = Roughness * Roughness;
    const float CosLobe = exp2(-3.321928 * Alpha * Alpha);
    const float CosOpen = sqrt(saturate(1.0 - AO));

    // Dividing by the open hemisphere's share stops grazing reflections on open ground reading as occluded.
    const float Unoccluded = SphericalCapIntersection(0.0, CosLobe, dot(N, R));
    const float Cone       = Unoccluded > 1e-4 ? saturate(SphericalCapIntersection(CosOpen, CosLobe, dot(BentN, R)) / Unoccluded) : AO;

    // A near mirror lobe is all or nothing against the cone, so it keeps the softer direction-free term.
    return lerp(ComputeSpecularOcclusion(NdotV, AO, Roughness), Cone, smoothstep(0.01, 0.09, Alpha));
}

// Colored multi-bounce ambient occlusion (Jimenez et al. 2016), diffuse only.
float3 AOMultiBounce(float Visibility, float3 DiffuseColor)
{
    float3 A =  2.0404 * DiffuseColor - 0.3324;
    float3 B = -4.7951 * DiffuseColor + 0.6417;
    float3 C =  2.7552 * DiffuseColor + 0.6903;
    return max(Visibility, ((Visibility * A + B) * Visibility + C) * Visibility);
}

// The mirror lobe returns separately so the translucent path can keep reflections un-premultiplied.
void EvaluateIBLAmbientSplit(float3 WorldPos, float3 N, float3 BentN, float3 V, float NdotV, float2 DFG, float3 Albedo, float3 F0, float Roughness, float Metallic, float AO,
                             out float3 OutDiffuse, out float3 OutSpecular)
{
    float3 R = reflect(-V, N);

    float3 kS     = FresnelSchlickRoughness(NdotV, F0, Roughness);
    float3 FssEss = kS * DFG.x + DFG.y;

    // Fdez-Aguera 2019 multi-scatter, with Ems the energy single scatter missed and Favg the cosine-averaged Fresnel.
    float  Ems    = saturate(1.0 - (DFG.x + DFG.y));
    float3 Favg   = F0 + (1.0 - F0) / 21.0;
    float3 FmsEms = (Ems * FssEss * Favg) / max(1.0 - Favg * Ems, 1e-4);

    float3 DiffuseColor = Albedo * (1.0 - Metallic);
    float3 kD           = DiffuseColor * (1.0 - FssEss - FmsEms);

    // The bent normal points at the open part of the hemisphere, so the sky arrives from where it can.
    float3 Irradiance = SampleDiffuseIrradiance(WorldPos, BentN);
    float3 PreFilt    = SampleSpecularRadiance(WorldPos, R, Roughness);

    float SpecAO = ComputeBentSpecularOcclusion(BentN, N, R, NdotV, AO, Roughness);

    OutSpecular = FssEss * PreFilt * SpecAO;
    // Only kD takes the colored bounce; FmsEms is specular energy, tinted by Favg rather than by albedo.
    OutDiffuse  = (FmsEms * AO + kD * AOMultiBounce(AO, DiffuseColor)) * Irradiance;
}

float3 EvaluateIBLAmbient(float3 WorldPos, float3 N, float3 BentN, float3 V, float NdotV, float2 DFG, float3 Albedo, float3 F0, float Roughness, float Metallic, float AO)
{
    float3 Diffuse, Specular;
    EvaluateIBLAmbientSplit(WorldPos, N, BentN, V, NdotV, DFG, Albedo, F0, Roughness, Metallic, AO, Diffuse, Specular);
    return Diffuse + Specular;
}
