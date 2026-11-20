#include "Includes/ParticleLighting.slang"

// The caster stage for the sun's shadow levels, keeping a fraction of pixels equal to the particle's opacity.

[shader("pixel")]
void PixelMain(FParticleVertexOutput In)
{
    BeginParticleDraw(In.DrawIndex);
    float Coverage = In.Color.a;
    if (In.TextureIndex != 0u)
    {
        Coverage *= FTexture2D(In.TextureIndex).Sample(Samplers::LinearWrap, In.UV).a;
    }
    else if (Pass().RenderMode == PARTICLE_RENDER_SPRITE)
    {
        const float2 Centered = In.UV * 2.0 - 1.0;
        const float  Disc     = saturate(1.0 - dot(Centered, Centered));
        Coverage *= Disc * Disc;
    }

    if (Coverage <= InterleavedGradientNoise(In.Position.xy))
    {
        discard;
    }

    // Particles move every frame, so they only ever land in the dynamic layer.
    WriteVirtualShadowLevelDepth(Pass().ShadowLevel, false, In.Position);
}
