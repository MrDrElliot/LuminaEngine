#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/Sky.slang"
#include "Includes/IBL.slang"

// GGX pre-filtered environment map (Karis 2013 split-sum "first sum"); each mip = a fixed roughness,
// sampled at runtime as mip = Roughness * (NumMips - 1). Dispatched once per mip with a Roughness push constant.
// No pass-local set: source cube via bindless cube SRV, output mip via bindless 2D-array UAV.

struct FPushConstants
{
    uint  SrcCubeSRV;
    uint  OutMipUAV;
    float Roughness;
    uint  NumSamples;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define PREFILTER_TILE 8

[shader("compute")]
[numthreads(PREFILTER_TILE, PREFILTER_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    RWTexture2DArray<float4> uOutMip = gRWTextures2DArray[Pass().OutMipUAV];

    uint Width, Height, Layers;
    uOutMip.GetDimensions(Width, Height, Layers);

    if (DTid.x >= Width || DTid.y >= Height || DTid.z >= Layers)
    {
        return;
    }

    float2 UV = (float2(DTid.xy) + 0.5) / float2(Width, Height);

    // Karis split-sum: V = N = R. Drops off-axis lobe anisotropy for a mip chain indexable by R.
    float3 N = CubeFaceDirection(DTid.z, UV);
    float3 R = N;
    float3 V = N;

    const uint NumSamples = max(Pass().NumSamples, 1u);
    float3     Sum        = float3(0.0, 0.0, 0.0);
    float      Weight     = 0.0;

    for (uint i = 0u; i < NumSamples; ++i)
    {
        float2 Xi    = Hammersley(i, NumSamples);
        float3 H     = ImportanceSampleGGX(Xi, N, Pass().Roughness);
        float3 L     = normalize(2.0 * dot(V, H) * H - V);
        float  NdotL = saturate(dot(N, L));

        if (NdotL > 0.0)
        {
            // NdotL weighting (UE4 / learnopengl): samples toward N count more, grazing less.
            // Sanitize per-sample so one Inf texel can't turn the whole accumulator into Inf/NaN.
            float3 Radiance = SanitizeRadiance(SampleTextureCubeLevel(Pass().SrcCubeSRV, SAMPLER_LINEAR_CLAMP, L, 0.0).rgb);
            // Accumulate in tonemapped space so a tiny bright source (city light, sun) hit by only a few
            // samples can't produce a blocky/sparkly firefly in the prefiltered mip.
            Sum    += FireflyTonemap(Radiance) * NdotL;
            Weight += NdotL;
        }
    }

    Sum = FireflyUntonemap(Sum / max(Weight, 1e-4));
    uOutMip[DTid] = float4(SanitizeRadiance(Sum), 1.0);
}
