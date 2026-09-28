#include "Includes/ParticleLighting.slang"

// Built-in sprite vertex stage, used by every sprite emitter that has no Particle material assigned.

[shader("vertex")]
FParticleVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID)
{
    FGPUParticle P;
    const FParticleSprite Sprite = ExpandParticleSprite(VertexIndex, P);
    if (!Sprite.bAlive)
    {
        return CulledParticleVertex();
    }

    FParticleVertexOutput Out;
    Out.UV           = Sprite.UV;
    Out.Color        = P.Color * float4(Pass().Tint.rgb * Pass().Tint.w, 1.0);
    Out.TextureIndex = Pass().TextureIndex;
    Out.Position     = ParticleToClip(Sprite.WorldPosition);

    // Each corner leans away from the camera, so the interpolated normal bulges the flat quad into a puff.
    const float2 Corner = kParticleQuadPositions[VertexIndex % 6u];
    Out.Normal = Sprite.AxisRight * Corner.x + Sprite.AxisUp * Corner.y + Sprite.Normal;
    LightParticleVertex(Out, P.Position, P.Size);
    return Out;
}
