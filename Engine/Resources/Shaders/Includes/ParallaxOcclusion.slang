#pragma once
#include "Includes/SceneGlobals.slang"

// Tatarchuk 2006 parallax occlusion mapping. Silhouettes stay flat and depth is unmodified.

// Must match the basis GetWorldNormal builds, or parallax shears against lighting. Rows are (T, B, N).
float3x3 BuildTangentFrame(float3 FragNormal, float4 FragTangent, out bool bDegenerate)
{
    // Every caller already passes a normalized N.
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

// The isotropic hardware mip formula, from the longer texel-space gradient.
float ParallaxMipLevel(uint HeightTextureIndex, float2 UVDdx, float2 UVDdy)
{
    float2 TexSize;
    FTexture2D(HeightTextureIndex).GetDimensions(TexSize.x, TexSize.y);

    float2 Dx = UVDdx * TexSize;
    float2 Dy = UVDdy * TexSize;
    return 0.5 * log2(max(max(dot(Dx, Dx), dot(Dy, Dy)), 1e-12));
}

// A fixed mip, since SampleGrad per march step costs rate and registers for a mip that barely changes.
float SampleParallaxHeight(uint HeightTextureIndex, uint SamplerIndex, float2 UV, float Mip)
{
    return FTexture2D(HeightTextureIndex).SampleLevel(FSampler(SamplerIndex), UV, Mip).r;
}

struct FParallaxResult
{
    float2 UV;       // Displaced UV that every other sample should use
    float  Height;   // Height at the intersection, 0 deep and 1 at the surface
    float  Weight;   // 1 is full parallax, 0 fades to flat normal mapping at distance
};

// ViewWS points from surface to camera. Most samples at grazing, the reverse of the paper's literal expression.
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

    // Global r.POM.* scaling over the authored values, where SampleScale 0 disables POM.
    float SampleScale = Scene().ParallaxSettings.SampleScale;
    if (SampleScale <= 0.0)
    {
        return Result;
    }
    MinSamples   = max(MinSamples * SampleScale, 1.0);
    MaxSamples   = max(MaxSamples * SampleScale, MinSamples);
    LODThreshold = LODThreshold + Scene().ParallaxSettings.LODBias;

    // Past the threshold mip the march only produces noise, so the offset fades to zero, which is plain normal mapping.
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

    // An untangented mesh or a ray parallel to the surface cannot march, so fall back to flat.
    if (bDegenerate || abs(ViewTS.z) < 1e-4)
    {
        Result.Weight = 0.0;
        return Result;
    }

    // Dividing by z makes the offset grow at grazing angles.
    float2 MaxOffset = (-ViewTS.xy / ViewTS.z) * HeightScale;

    // The geometric normal, since the mapped one is what this computes.
    float  NdotV    = saturate(dot(NormalWS, ViewWS));
    int    NumSteps = (int)max(lerp(MaxSamples, MinSamples, NdotV), 1.0);
    float  StepSize = 1.0 / (float)NumSteps;
    float2 StepUV   = MaxOffset * StepSize;

    float  RayHeight      = 1.0;                 // Marches down from the polygon plane
    float2 CurrentOffset  = float2(0.0, 0.0);
    float2 PreviousOffset = float2(0.0, 0.0);
    float  CurrentHeight  = 1.0;
    float  PreviousHeight = 1.0;

    // Linear search keeps fetches non-dependent and cache coherent, which is why the paper drops the binary search.
    int Step = 0;
    while (Step < NumSteps)
    {
        CurrentHeight = SampleParallaxHeight(HeightTextureIndex, SamplerIndex, UV + CurrentOffset, Mip);

        if (CurrentHeight > RayHeight)
        {
            // Intersect the segment between the two height samples to avoid stair-stepping.
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

// Samples past the first blocker, weighted by distance, to approximate the penumbra. Sun only, returns 1 when lit.
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
    // Back-facing surfaces are already dark from N.L, and tracing them would double-darken the terminator.
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

    // The light ray climbs from the intersection height back to the polygon plane.
    float2 MaxOffset = (LightTS.xy / LightTS.z) * HeightScale * (1.0 - SurfaceHeight);
    float  StepSize  = 1.0 / (float)Steps;

    float Occlusion = 0.0;
    for (int i = 1; i <= Steps; ++i)
    {
        float  T      = (float)i * StepSize;
        float2 Offset = MaxOffset * T;
        float  Height = SampleParallaxHeight(HeightTextureIndex, SamplerIndex, DisplacedUV + Offset, Mip);

        float RayHeight = SurfaceHeight + (1.0 - SurfaceHeight) * T;
        float Blocked   = max(Height - RayHeight, 0.0);

        // Max rather than sum keeps one tall blocker from being outvoted by many shallow ones.
        Occlusion = max(Occlusion, Blocked * (1.0 - T));
    }

    return saturate(1.0 - Occlusion * Softness);
}
