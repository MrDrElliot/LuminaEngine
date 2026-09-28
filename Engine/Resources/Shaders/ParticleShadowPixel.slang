#include "Includes/ParticleLighting.slang"

// Depth-only caster stage for the sun cascades, keeping a fraction of pixels equal to the particle's opacity.

[shader("pixel")]
void PixelMain(FParticleVertexOutput In)
{
    float Coverage = In.Color.a;
    if (In.TextureIndex != 0u)
    {
        Coverage *= SampleTexture2D(In.TextureIndex, SAMPLER_LINEAR_WRAP, In.UV).a;
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
}
