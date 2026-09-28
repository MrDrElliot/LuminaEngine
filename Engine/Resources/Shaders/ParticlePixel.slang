#include "Includes/ParticleSpriteCommon.slang"

struct PSInput
{
    float4                  Color        : PARTICLE_COLOR;
    float2                  UV           : TEXCOORD0;
    nointerpolation uint    TextureIndex : TEXTURE_INDEX;
    float3                  Normal       : PARTICLE_NORMAL;
    float3                  Ambient      : PARTICLE_AMBIENT;
    float3                  SunLight     : PARTICLE_SUN;
    float4                  Position     : SV_Position;
};

struct PSOutput
{
    float4 Color : SV_Target0;
};

// Soft radial falloff used when no texture is assigned. Keeps particles from
// rendering as hard squares and gives decent results for smoke/fire defaults.
float DefaultSprite(float2 uv)
{
    float2 p = uv * 2.0 - 1.0;
    float d  = saturate(1.0 - dot(p, p));
    return d * d;
}

[shader("pixel")]
PSOutput PixelMain(PSInput In)
{
    float4 texel;
    if (In.TextureIndex != 0u)
    {
        texel = SampleTexture2D(In.TextureIndex, SAMPLER_LINEAR_WRAP, In.UV);
    }
    else
    {
        float mask = DefaultSprite(In.UV);
        texel = float4(1.0, 1.0, 1.0, mask);
    }

    float4 outColor = texel * In.Color;

    if (IsParticleLit())
    {
        // Half-Lambert wraps light around the puff, which reads as scattering through smoke rather than a hard terminator.
        const float Wrap = saturate(dot(normalize(In.Normal), GetSunDirection()) * 0.5 + 0.5);
        outColor.rgb *= In.Ambient + In.SunLight * (Wrap * INV_PI);
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
