#include "Includes/ParticleSpriteCommon.slang"
#include "Includes/ShadowSampling.slang"

// Built-in sprite vertex stage, used by every emitter that has no Particle material assigned.

struct VertexOutput
{
    float4                  Color        : PARTICLE_COLOR;
    float2                  UV           : TEXCOORD0;
    nointerpolation uint    TextureIndex : TEXTURE_INDEX;
    float3                  Normal       : PARTICLE_NORMAL;
    float3                  Ambient      : PARTICLE_AMBIENT;
    float3                  SunLight     : PARTICLE_SUN;
    float4                  Position     : SV_Position;
};

[shader("vertex")]
VertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID)
{
    FGPUParticle P;
    const FParticleSprite Sprite = ExpandParticleSprite(VertexIndex, P);

    VertexOutput Out;

    // Dead particles collapse to a degenerate clip-culled point.
    if (!Sprite.bAlive)
    {
        Out.Position     = float4(2.0, 2.0, 2.0, 1.0);
        Out.Color        = float4(0.0, 0.0, 0.0, 0.0);
        Out.UV           = float2(0.0, 0.0);
        Out.TextureIndex = 0;
        Out.Normal       = float3(0.0, 1.0, 0.0);
        Out.Ambient      = float3(0.0, 0.0, 0.0);
        Out.SunLight     = float3(0.0, 0.0, 0.0);
        return Out;
    }

    Out.UV           = Sprite.UV;
    Out.Color        = P.Color * float4(Pass().Tint.rgb * Pass().Tint.w, 1.0);
    Out.TextureIndex = Pass().TextureIndex;
    Out.Position     = WorldToClip(Sprite.WorldPosition);

    // Each corner leans away from the camera, so the interpolated normal bulges the flat quad into a puff.
    const float2 Corner = kParticleQuadPositions[VertexIndex % 6u];
    Out.Normal   = Sprite.AxisRight * Corner.x + Sprite.AxisUp * Corner.y + Sprite.Normal;
    Out.Ambient  = float3(0.0, 0.0, 0.0);
    Out.SunLight = float3(0.0, 0.0, 0.0);
    if (IsParticleLit())
    {
        // Lit once per particle at its center, so a puff shades as one body and the cascade lookup stays per vertex.
        Out.Ambient = GetVolumetricAmbientRadiance();
        if (GetNumLights() > 0u && HasFlag(GetLightAt(0).Flags, ELightFlags::Directional))
        {
            const FLight Sun = GetLightAt(0);
            Out.SunLight = UnpackColor(Sun.Color).rgb * Sun.Intensity * SampleSunCascadeByContainment(Sun, P.Position);
        }
    }
    return Out;
}
