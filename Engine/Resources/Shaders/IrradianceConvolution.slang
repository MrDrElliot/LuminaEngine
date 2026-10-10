#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Sky.slang"
#include "Includes/IBL.slang"

// Cosine-weighted sampling (Malley's method) reduces Irradiance(N)/pi to the plain mean of L.

struct FPushConstants
{
    FTextureCube      SrcCube;
    FRWTexture2DArray OutCube;
    uint              _Pad0;
    uint              _Pad1;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define IRRADIANCE_TILE 8

[shader("compute")]
[numthreads(IRRADIANCE_TILE, IRRADIANCE_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    FRWTexture2DArray uOutCube = Pass().OutCube;

    uint Width, Height, Layers;
    uOutCube.GetDimensions(Width, Height, Layers);

    if (DTid.x >= Width || DTid.y >= Height || DTid.z >= Layers)
    {
        return;
    }

    float2 UV = (float2(DTid.xy) + 0.5) / float2(Width, Height);
    float3 N  = CubeFaceDirection(DTid.z, UV);

    float3 Up        = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 Tangent   = normalize(cross(Up, N));
    float3 Bitangent = cross(N, Tangent);

    const uint NumSamples = 1024u;
    float3     Sum        = float3(0.0, 0.0, 0.0);

    for (uint i = 0u; i < NumSamples; ++i)
    {
        float2 Xi       = Hammersley(i, NumSamples);
        float  Phi      = 2.0 * PI * Xi.x;
        float  CosTheta = sqrt(1.0 - Xi.y);
        float  SinTheta = sqrt(Xi.y);

        float3 LocalDir = float3(SinTheta * cos(Phi), SinTheta * sin(Phi), CosTheta);
        float3 L        = normalize(LocalDir.x * Tangent + LocalDir.y * Bitangent + LocalDir.z * N);

        // Tonemapped accumulation suppresses fireflies, same as the prefilter.
        Sum += FireflyTonemap(SanitizeRadiance(Pass().SrcCube.SampleLevel(Samplers::LinearClamp, L, 0.0).rgb));
    }

    float3 Irradiance = FireflyUntonemap(Sum / float(NumSamples));
    uOutCube[DTid] = float4(SanitizeRadiance(Irradiance), 1.0);
}
