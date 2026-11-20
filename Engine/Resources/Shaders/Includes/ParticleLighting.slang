#pragma once
#include "ParticleSpriteCommon.slang"
#include "ShadowSampling.slang"
#include "LocalIrradiance.slang"

// Light a diffuse particle receives, split so the pixel stage can shape the sun term by a normal.
struct FParticleLight
{
    float3 Ambient;
    float3 Sun;
};

FParticleLight ParticleLightAt(float3 WorldPos)
{
    FParticleLight Light;
    // A puff faces local lights on half its surface on average, hence the half.
    Light.Ambient = GetVolumetricAmbientRadiance() + ClusteredLocalIrradiance(WorldPos) * (0.5 * INV_PI);
    Light.Sun     = float3(0.0, 0.0, 0.0);
    if (GetNumLights() > 0u && HasFlag(GetLightAt(0).Flags, ELightFlags::Directional))
    {
        const FLight Sun = GetLightAt(0);
        Light.Sun = UnpackColor(Sun.Color).rgb * Sun.Intensity * SampleSunShadowPoint(Sun, WorldPos)
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
    nointerpolation uint    DrawIndex    : PARTICLE_DRAW;
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
    Out.DrawIndex    = 0u;
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
