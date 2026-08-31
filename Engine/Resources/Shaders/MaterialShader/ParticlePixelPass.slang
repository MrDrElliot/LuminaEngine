#include "Includes/ParticleSpriteCommon.slang"

// Particle material pixel template; sprites are unlit, so Emissive is the color and Opacity the coverage.

[shader("fragment")]
float4 FragmentMain(FParticleMaterialVertexOutput Input) : SV_Target0
{
    // Material graph aliases, matching the names the vertex template declares.
    uint   MaterialIndex = Pass().MaterialIndex;
    float2 UV0           = Input.UV;
    float3 WorldPosition = Input.WorldPos;
    float3 WorldNormal   = normalize(Input.Normal);
    float3 ViewPosition  = Input.ViewPos;
    float4 VertexColor   = Input.Color;
    float4 WorldTangent  = Input.Tangent;
    uint   EntityID      = 0;
    // A sprite has one parameterization; set 1 aliases it.
    float2 UV1           = UV0;
    float2 UV0_DDX       = ddx(UV0);
    float2 UV0_DDY       = ddy(UV0);
    float2 UV1_DDX       = ddx(UV1);
    float2 UV1_DDY       = ddy(UV1);
    float3 WorldPosition_DDX = ddx(WorldPosition);
    float3 WorldPosition_DDY = ddy(WorldPosition);
    float4 VertexColor_DDX   = ddx(VertexColor);
    float4 VertexColor_DDY   = ddy(VertexColor);

    float4 ParticleColor      = Input.Color;
    float3 ParticlePosition   = Input.Center;
    float3 ParticleVelocity   = Input.Velocity;
    float2 ParticleSize       = Input.SizeLifeRand.xy;
    float  ParticleLife       = Input.SizeLifeRand.z;
    float  ParticleSeedRandom = Input.SizeLifeRand.w;

    RequestMaterialResolution(MaterialIndex, UV0_DDX, UV0_DDY, UV1_DDX, UV1_DDY);
    $MATERIAL_INPUTS

    float  Alpha = saturate(Material.Opacity);
    float3 Color = Material.Emissive;

#ifdef MASKED

    if (Alpha < Materials()[MaterialIndex].OpacityClipValue)
    {
        discard;
    }
    return float4(Color, 1.0);

#elif defined(MODULATE)

    // Blended as DstColor * Src, so opacity has to fade the color toward the multiplicative identity.
    return float4(lerp(float3(1.0, 1.0, 1.0), Color, Alpha), 1.0);

#else

    if (Alpha <= 0.001)
    {
        discard;
    }
    return float4(Color, Alpha);

#endif
}
