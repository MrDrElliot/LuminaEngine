#pragma once
#include "SceneGlobals.slang"

// Asymmetry of the forward lobe a falling drop throws light into, the glint seen looking toward a lamp through rain.
static const float kForwardScatterG = 0.75;

// Henyey-Greenstein over the isotropic phase, 1 at right angles and far above it looking into the light.
float ForwardScatterRatio(float CosTheta)
{
    const float G2 = kForwardScatterG * kForwardScatterG;
    return (1.0 - G2) / pow(max(1.0 + G2 - 2.0 * kForwardScatterG * CosTheta, 1e-4), 1.5);
}

// Irradiance from the point and spot lights the cluster grid lists at WorldPos, with no facing term unless ForwardScatter adds a lobe toward ToEye.
float3 ClusteredLocalIrradiance(float3 WorldPos, float MinDistance = 0.01, float3 ToEye = float3(0.0, 0.0, 0.0), float ForwardScatter = 0.0)
{
    const float4 Clip = WorldToClip(WorldPos);
    if (Clip.w <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float2 ScreenPos = ((Clip.xy / Clip.w) * 0.5 + 0.5) * float2(GetScreenSize());
    const FLightCell Cell  = GetLightCell(WorldToView(WorldPos).z, ScreenPos);
    const uint2  Words     = GetLightCellWords(Cell);
    const uint   NumLights = GetNumLights();

    float3 Irradiance = float3(0.0, 0.0, 0.0);
    for (uint Word = Words.x; Word < Words.y; ++Word)
    {
        uint Pending = GetLightCellMaskWord(Cell, Word);
        while (Pending != 0u)
        {
            const uint LightIndex = Word * 32u + firstbitlow(Pending);
            Pending &= Pending - 1u;
            if (LightIndex >= NumLights)
            {
                continue;
            }

            const FLight L = GetLightAt(LightIndex);
            if (HasFlag(L.Flags, ELightFlags::Directional))
            {
                continue;
            }

            const float3 ToLight  = L.Position - WorldPos;
            const float  Distance = length(ToLight);
            if (Distance > L.Radius)
            {
                continue;
            }

            float Attenuation = 1.0 / max(Distance * Distance, MinDistance * MinDistance);
            Attenuation *= 1.0 - smoothstep(clamp(L.Falloff, 0.0, 1.0), 1.0, Distance / L.Radius);

            if (HasFlag(L.Flags, ELightFlags::Spot))
            {
                const float CosAngle = dot(normalize(L.Direction), ToLight / max(Distance, 1e-4));
                Attenuation *= saturate((CosAngle - L.Angles.y) / max(L.Angles.x - L.Angles.y, 1e-3));
            }
            else if (HasFlag(L.Flags, ELightFlags::Area))
            {
                Attenuation *= saturate(dot(normalize(L.Direction), ToLight / max(Distance, 1e-4)));
            }

            if (ForwardScatter > 0.0)
            {
                Attenuation *= 1.0 + ForwardScatter * ForwardScatterRatio(dot(-ToLight / max(Distance, 1e-4), ToEye));
            }

            Irradiance += UnpackColor(L.Color).rgb * L.Intensity * Attenuation;
        }
    }
    return Irradiance;
}
