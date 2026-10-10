#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Sky.slang"
#include "Includes/IBL.slang"

// Each mip is a fixed roughness; probes write into the cube array at DstLayerOffset = ProbeSlice * 6.

struct FPushConstants
{
    FTextureCube      SrcCube;
    FRWTexture2DArray OutMip;
    float             Roughness;
    uint              NumSamples;
    uint              DstLayerOffset;
    uint              _Pad0;
    uint              _Pad1;
    uint              _Pad2;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define PREFILTER_TILE 8

[shader("compute")]
[numthreads(PREFILTER_TILE, PREFILTER_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    FRWTexture2DArray uOutMip = Pass().OutMip;

    uint Width, Height, Layers;
    uOutMip.GetDimensions(Width, Height, Layers);

    // DTid.z is the cube face, offset into the destination array.
    const uint DstLayer = DTid.z + Pass().DstLayerOffset;

    if (DTid.x >= Width || DTid.y >= Height || DTid.z >= 6u || DstLayer >= Layers)
    {
        return;
    }

    float2 UV = (float2(DTid.xy) + 0.5) / float2(Width, Height);

    // Karis split-sum assumes V = N = R, trading lobe anisotropy for a mip chain indexable by R.
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
            // Sanitized per sample so one Inf texel cannot poison the accumulator.
            float3 Radiance = SanitizeRadiance(Pass().SrcCube.SampleLevel(Samplers::LinearClamp, L, 0.0).rgb);
            // Tonemapped accumulation keeps a tiny bright source hit by few samples from becoming a firefly.
            Sum    += FireflyTonemap(Radiance) * NdotL;
            Weight += NdotL;
        }
    }

    Sum = FireflyUntonemap(Sum / max(Weight, 1e-4));
    uOutMip[uint3(DTid.xy, DstLayer)] = float4(SanitizeRadiance(Sum), 1.0);
}
