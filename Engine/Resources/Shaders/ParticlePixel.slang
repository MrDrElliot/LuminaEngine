#include "Includes/ParticleLighting.slang"

struct PSOutput
{
    float4 Color : SV_Target0;
};

// Coverage when no texture is assigned, a soft disc for sprites, a soft band for ribbons and solid for meshes.
float DefaultCoverage(float2 UV)
{
    if (Pass().RenderMode == PARTICLE_RENDER_MESH)
    {
        return 1.0;
    }
    if (Pass().RenderMode == PARTICLE_RENDER_RIBBON)
    {
        const float Across = UV.y * 2.0 - 1.0;
        return saturate(1.0 - Across * Across);
    }

    const float2 Centered = UV * 2.0 - 1.0;
    const float  Disc     = saturate(1.0 - dot(Centered, Centered));
    return Disc * Disc;
}

[shader("pixel")]
PSOutput PixelMain(FParticleVertexOutput In)
{
    float4 texel;
    if (In.TextureIndex != 0u)
    {
        texel = FTexture2D(In.TextureIndex).Sample(Samplers::LinearWrap, In.UV);
    }
    else
    {
        texel = float4(1.0, 1.0, 1.0, DefaultCoverage(In.UV));
    }

    float4 outColor = texel * In.Color;

    if (IsParticleLit())
    {
        // A mesh is a real surface, while half-Lambert wraps light around a puff the way it scatters through smoke.
        const float NdotL  = dot(normalize(In.Normal), GetSunDirection());
        const float Facing = Pass().RenderMode == PARTICLE_RENDER_MESH ? saturate(NdotL) : saturate(NdotL * 0.5 + 0.5);
        outColor.rgb *= In.Ambient + In.SunLight * (Facing * INV_PI);
    }

    const float Fade = ParticleSoftFade(In.Position);
    outColor.a *= Fade;
    if (ParticleBlendMode() == PARTICLE_BLEND_PREMULTIPLIED)
    {
        outColor.rgb *= Fade;
    }
    else if (ParticleBlendMode() == PARTICLE_BLEND_MULTIPLY)
    {
        outColor.rgb = lerp(float3(1.0, 1.0, 1.0), outColor.rgb, Fade);
    }

    if (outColor.a <= 0.001)
    {
        discard;
    }

    PSOutput Out;
    Out.Color = outColor;
    return Out;
}
