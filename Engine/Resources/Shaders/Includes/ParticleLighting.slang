#pragma once
#include "ParticleSpriteCommon.slang"
#include "ShadowSampling.slang"

// Light a diffuse particle receives, split so the pixel stage can shape the sun term by a normal.
struct FParticleLight
{
    float3 Ambient;
    float3 Sun;
};

// Irradiance from the point and spot lights the cluster grid lists at WorldPos, with no facing term.
float3 ParticleLocalIrradiance(float3 WorldPos)
{
    const float4 Clip = WorldToClip(WorldPos);
    if (Clip.w <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float2 ScreenPos = ((Clip.xy / Clip.w) * 0.5 + 0.5) * float2(GetScreenSize());
    const uint   Cluster   = ComputeClusterIndex(WorldToView(WorldPos).z, ScreenPos);
    const uint   MaskWords = GetClusterMaskWords();
    const uint2  Words     = GetClusterWordRange(Cluster);
    const uint   NumLights = GetNumLights();

    float3 Irradiance = float3(0.0, 0.0, 0.0);
    for (uint Word = Words.x; Word < Words.y; ++Word)
    {
        uint Pending = GetClusterMaskWord(Cluster, Word, MaskWords);
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

            float Attenuation = 1.0 / max(Distance * Distance, 1e-4);
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

            Irradiance += UnpackColor(L.Color).rgb * L.Intensity * Attenuation;
        }
    }
    return Irradiance;
}

FParticleLight ParticleLightAt(float3 WorldPos)
{
    FParticleLight Light;
    // A puff faces local lights on half its surface on average, hence the half.
    Light.Ambient = GetVolumetricAmbientRadiance() + ParticleLocalIrradiance(WorldPos) * (0.5 * INV_PI);
    Light.Sun     = float3(0.0, 0.0, 0.0);
    if (GetNumLights() > 0u && HasFlag(GetLightAt(0).Flags, ELightFlags::Directional))
    {
        const FLight Sun = GetLightAt(0);
        Light.Sun = UnpackColor(Sun.Color).rgb * Sun.Intensity * SampleSunCascadeByContainment(Sun, WorldPos)
                  * SampleDirectionalCloudShadow(WorldPos);
    }
    return Light;
}

// The interpolants every built-in particle vertex stage writes and ParticlePixel.slang reads.
struct FParticleVertexOutput
{
    float4                  Color        : PARTICLE_COLOR;
    float2                  UV           : TEXCOORD0;
    nointerpolation uint    TextureIndex : TEXTURE_INDEX;
    float3                  Normal       : PARTICLE_NORMAL;
    float3                  Ambient      : PARTICLE_AMBIENT;
    float3                  SunLight     : PARTICLE_SUN;
    float4                  Position     : SV_Position;
};

// Dead particles collapse to a point outside the clip volume.
FParticleVertexOutput CulledParticleVertex()
{
    FParticleVertexOutput Out;
    Out.Position     = float4(2.0, 2.0, 2.0, 1.0);
    Out.Color        = float4(0.0, 0.0, 0.0, 0.0);
    Out.UV           = float2(0.0, 0.0);
    Out.TextureIndex = 0;
    Out.Normal       = float3(0.0, 1.0, 0.0);
    Out.Ambient      = float3(0.0, 0.0, 0.0);
    Out.SunLight     = float3(0.0, 0.0, 0.0);
    return Out;
}

// Lit once at the particle so it shades as one body, sampled a size toward the sun so a caster does not shadow itself.
void LightParticleVertex(inout FParticleVertexOutput Out, float3 ParticleCenter, float ParticleSize)
{
    Out.Ambient  = float3(0.0, 0.0, 0.0);
    Out.SunLight = float3(0.0, 0.0, 0.0);
    if (IsParticleLit() && Pass().bShadowPass == 0u)
    {
        const FParticleLight Light = ParticleLightAt(ParticleCenter + normalize(GetSunDirection()) * ParticleSize);
        Out.Ambient  = Light.Ambient;
        Out.SunLight = Light.Sun;
    }
}
