#include "Includes/ParticleSpriteCommon.slang"

// Particle material vertex template; expands the sprite, then lets the graph displace the corner.

[shader("vertex")]
FParticleMaterialVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID)
{
    FGPUParticle P;
    const FParticleSprite Sprite = ExpandParticleSprite(VertexIndex, P);

    FParticleMaterialVertexOutput Out;

    // Dead particles collapse to a degenerate clip-culled point.
    if (!Sprite.bAlive)
    {
        Out.Color        = float4(0.0, 0.0, 0.0, 0.0);
        Out.UV           = float2(0.0, 0.0);
        Out.WorldPos     = float3(0.0, 0.0, 0.0);
        Out.ViewPos      = float3(0.0, 0.0, 0.0);
        Out.Normal       = float3(0.0, 0.0, 1.0);
        Out.Tangent      = float4(1.0, 0.0, 0.0, 1.0);
        Out.Velocity     = float3(0.0, 0.0, 0.0);
        Out.Center       = float3(0.0, 0.0, 0.0);
        Out.SizeLifeRand = float4(0.0, 0.0, 0.0, 0.0);
        Out.Position     = float4(2.0, 2.0, 2.0, 1.0);
        return Out;
    }

    const float4 SpriteColor = P.Color * float4(Pass().Tint.rgb * Pass().Tint.w, 1.0);

    // Material graph aliases, matching the names the pixel template declares.
    float3 WorldPosition = Sprite.WorldPosition;
    float3 WorldNormal   = Sprite.Normal;
    float4 WorldTangent  = float4(Sprite.AxisRight, 1.0);
    float2 UV0           = Sprite.UV;
    // A sprite has one parameterization; set 1 aliases it, as the pixel template also does.
    float2 UV1           = UV0;
    float4 VertexColor   = SpriteColor;
    uint   MaterialIndex = Pass().MaterialIndex;
    uint   EntityID      = 0;
    float3 ViewPosition  = mul(GetCameraView(), float4(WorldPosition, 1.0)).xyz;

    float4 ParticleColor      = SpriteColor;
    float3 ParticlePosition   = P.Position;
    float3 ParticleVelocity   = P.Velocity;
    float2 ParticleSize       = Sprite.Extent;
    float  ParticleLife       = ParticleRelativeTime(P);
    float  ParticleSeedRandom = ParticleRandom(P);

    FMaterialVertexInputs Material;
    $MATERIAL_VERTEX_INPUTS
    WorldPosition += Material.WorldPositionOffset;

    Out.Color        = SpriteColor;
    Out.UV           = Sprite.UV;
    Out.WorldPos     = WorldPosition;
    Out.ViewPos      = mul(GetCameraView(), float4(WorldPosition, 1.0)).xyz;
    Out.Normal       = Sprite.Normal;
    Out.Tangent      = float4(Sprite.AxisRight, 1.0);
    Out.Velocity     = P.Velocity;
    Out.Center       = P.Position;
    Out.SizeLifeRand = float4(ParticleSize, ParticleLife, ParticleSeedRandom);
    Out.Position     = WorldToClip(WorldPosition);
    return Out;
}
