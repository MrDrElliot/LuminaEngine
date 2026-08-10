#pragma once
#include "Includes/SceneGlobals.slang"

// Parallax Occlusion Mapping (Tatarchuk 2006): per-pixel ray trace of a tangent-space height field,
// producing a UV offset every other fetch uses. Silhouettes stay flat and depth is unmodified.

// MUST stay byte-identical to what GetWorldNormal builds: POM offsets UVs along this basis and the
// normal map decodes through it, so any drift shears parallax against lighting. Rows are (T, B, N).
float3x3 BuildTangentFrame(float3 FragNormal, float4 FragTangent, out bool bDegenerate)
{
    // N is assumed unit-length: every caller passes the already-normalized interpolated normal.
    float3 N = FragNormal;

    float3 T = FragTangent.xyz - N * dot(FragTangent.xyz, N);
    float TLen2 = dot(T, T);

    bDegenerate = TLen2 < 1e-8;
    if (bDegenerate)
    {
        float3 Up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
        T = normalize(cross(Up, N));
    }
    else
    {
        T *= rsqrt(TLen2);
    }

    float3 B = cross(N, T) * FragTangent.w;
    return float3x3(T, B, N);
}

float3x3 BuildTangentFrame(float3 FragNormal, float4 FragTangent)
{
    bool bDegenerate;
    return BuildTangentFrame(FragNormal, FragTangent, bDegenerate);
}

// Mip level this pixel would sample the height field at, from the UV gradients and the texture size.
// The standard isotropic formula the hardware uses: log2 of the longer texel-space gradient.
float ParallaxMipLevel(uint HeightTextureIndex, float2 UVDdx, float2 UVDdy)
{
    float2 TexSize;
    gTextures2D[HeightTextureIndex].GetDimensions(TexSize.x, TexSize.y);

    float2 Dx = UVDdx * TexSize;
    float2 Dy = UVDdy * TexSize;
    return 0.5 * log2(max(max(dot(Dx, Dx), dot(Dy, Dy)), 1e-12));
}

// Fixed mip on purpose: the march takes up to MaxSamples fetches, and SampleGrad per iteration costs
// rate and registers for a mip that barely changes over the offset. Red channel.
float SampleParallaxHeight(uint HeightTextureIndex, uint SamplerIndex, float2 UV, float Mip)
{
    return SampleTexture2DLevel(HeightTextureIndex, SamplerIndex, UV, Mip).r;
}

struct FParallaxResult
{
    float2 UV;       // displaced texture coordinate; feed this to every other sample
    float  Height;   // height field value at the intersection, 0 (deep) .. 1 (surface)
    float  Weight;   // 1 = full parallax, 0 = fully faded to flat normal mapping at distance
};

// ViewWS is the normalized surface->camera vector. Uses lerp(Max, Min, N.V) -- most samples at
// grazing, fewest head-on -- which is the opposite of the paper's expression as literally written.
FParallaxResult ParallaxOcclusion(
    uint   HeightTextureIndex,
    uint   SamplerIndex,
    float2 UV,
    float3 ViewWS,
    float3 NormalWS,
    float4 TangentWS,
    float  HeightScale,
    float  MinSamples,
    float  MaxSamples,
    float  LODThreshold,
    float2 UVDdx,
    float2 UVDdy)
{
    FParallaxResult Result;
    Result.UV     = UV;
    Result.Height = 1.0;
    Result.Weight = 0.0;

    // Global quality scaling (r.POM.*), applied on top of the material's authored values so one CVar
    // dials every POM material without recompiling any of them. SampleScale 0 disables the feature.
    float SampleScale = Scene().ParallaxSettings.SampleScale;
    if (SampleScale <= 0.0)
    {
        return Result;
    }
    MinSamples   = max(MinSamples * SampleScale, 1.0);
    MaxSamples   = max(MaxSamples * SampleScale, MinSamples);
    LODThreshold = LODThreshold + Scene().ParallaxSettings.LODBias;

    // Above the threshold mip the height field is smaller than the pixel footprint, so the march can only
    // produce noise. Blends the OFFSET toward zero: a zero offset IS normal mapping, so endpoints match.
    float Mip    = ParallaxMipLevel(HeightTextureIndex, UVDdx, UVDdy);
    float Weight = saturate(LODThreshold - Mip);
    if (Weight <= 0.0)
    {
        return Result;
    }
    Result.Weight = Weight;

    bool     bDegenerate;
    float3x3 Frame  = BuildTangentFrame(NormalWS, TangentWS, bDegenerate);
    float3   ViewTS = mul(Frame, ViewWS);

    // No tangent frame tied to the UV layout (untangented mesh), or a ray parallel to the surface (the
    // offset diverges). Neither can produce a meaningful march; fall back to flat.
    if (bDegenerate || abs(ViewTS.z) < 1e-4)
    {
        Result.Weight = 0.0;
        return Result;
    }

    // Maximum UV displacement, reached when the ray descends the full field. Dividing the lateral
    // component by z is the perspective term that makes the offset grow at grazing angles.
    float2 MaxOffset = (-ViewTS.xy / ViewTS.z) * HeightScale;

    // Dynamic sampling rate, from the GEOMETRIC normal (not the mapped one -- the mapped normal is what
    // we are on our way to computing).
    float  NdotV    = saturate(dot(NormalWS, ViewWS));
    int    NumSteps = (int)max(lerp(MaxSamples, MinSamples, NdotV), 1.0);
    float  StepSize = 1.0 / (float)NumSteps;
    float2 StepUV   = MaxOffset * StepSize;

    float  RayHeight      = 1.0;                 // ray marches down from the polygon plane
    float2 CurrentOffset  = float2(0.0, 0.0);
    float2 PreviousOffset = float2(0.0, 0.0);
    float  CurrentHeight  = 1.0;
    float  PreviousHeight = 1.0;

    // Linear search with dynamic early-out. Only regular (non-dependent) fetches along a straight UV
    // line, so this stays texture-cache coherent -- the reason the paper drops the binary search.
    int Step = 0;
    while (Step < NumSteps)
    {
        CurrentHeight = SampleParallaxHeight(HeightTextureIndex, SamplerIndex, UV + CurrentOffset, Mip);

        if (CurrentHeight > RayHeight)
        {
            // Solve the ray against the LINE SEGMENT joining the two height samples rather than accepting the
            // sample point. This is the paper's high-precision intersection: no stair-stepping, no depth bias.
            float Delta1 = CurrentHeight - RayHeight;
            float Delta2 = (RayHeight + StepSize) - PreviousHeight;
            float Ratio  = Delta1 / max(Delta1 + Delta2, 1e-6);

            CurrentOffset = lerp(CurrentOffset, PreviousOffset, Ratio);
            CurrentHeight = lerp(CurrentHeight, PreviousHeight, Ratio);
            break;
        }

        Step++;
        RayHeight     -= StepSize;
        PreviousOffset = CurrentOffset;
        PreviousHeight = CurrentHeight;
        CurrentOffset += StepUV;
    }

    Result.UV     = UV + CurrentOffset * Weight;
    Result.Height = CurrentHeight;
    return Result;
}

// Keeps sampling PAST the first blocker and weights each by distance, approximating the penumbra;
// stopping at the first blocker gives hard aliased edges. One light only: the sun. Returns 1 = lit.
float ParallaxSelfShadow(
    uint   HeightTextureIndex,
    uint   SamplerIndex,
    float2 DisplacedUV,
    float  SurfaceHeight,
    float3 LightWS,
    float3 NormalWS,
    float4 TangentWS,
    float  HeightScale,
    float  NumSamples,
    float  Softness,
    float  Mip)
{
    // Surfaces facing away from the light are already fully shadowed by N.L; tracing them is wasted
    // work and would double-darken the terminator. The paper gates on exactly this test.
    float NdotL = dot(NormalWS, LightWS);
    if (NdotL <= 0.0)
    {
        return 1.0;
    }

    float3x3 Frame   = BuildTangentFrame(NormalWS, TangentWS);
    float3   LightTS = mul(Frame, LightWS);
    if (LightTS.z < 1e-4)
    {
        return 1.0;
    }

    float ShadowScale = Scene().ParallaxSettings.ShadowSampleScale;
    if (ShadowScale <= 0.0)
    {
        return 1.0;
    }

    int Steps = (int)max(NumSamples * ShadowScale, 1.0);

    // The light ray climbs from the intersection height back to the polygon plane; walk that span.
    float2 MaxOffset = (LightTS.xy / LightTS.z) * HeightScale * (1.0 - SurfaceHeight);
    float  StepSize  = 1.0 / (float)Steps;

    float Occlusion = 0.0;
    for (int i = 1; i <= Steps; ++i)
    {
        float  T      = (float)i * StepSize;
        float2 Offset = MaxOffset * T;
        float  Height = SampleParallaxHeight(HeightTextureIndex, SamplerIndex, DisplacedUV + Offset, Mip);

        // Height of the light ray at this step, and how far the height field pokes above it.
        float RayHeight = SurfaceHeight + (1.0 - SurfaceHeight) * T;
        float Blocked   = max(Height - RayHeight, 0.0);

        // A near blocker contributes sharp strong occlusion, a far one a weak diffuse contribution. Max
        // rather than sum keeps one tall blocker from being out-voted by a run of shallow ones.
        Occlusion = max(Occlusion, Blocked * (1.0 - T));
    }

    return saturate(1.0 - Occlusion * Softness);
}
