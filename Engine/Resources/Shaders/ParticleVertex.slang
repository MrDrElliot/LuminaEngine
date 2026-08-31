#include "Includes/ParticleSpriteCommon.slang"

// Built-in sprite vertex stage, used by every emitter that has no Particle material assigned.

struct VertexOutput
{
    float4                  Color        : PARTICLE_COLOR;
    float2                  UV           : TEXCOORD0;
    nointerpolation uint    TextureIndex : TEXTURE_INDEX;
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
        return Out;
    }

    Out.UV           = Sprite.UV;
    Out.Color        = P.Color * float4(Pass().Tint.rgb * Pass().Tint.w, 1.0);
    Out.TextureIndex = Pass().TextureIndex;
    Out.Position     = WorldToClip(Sprite.WorldPosition);
    return Out;
}
