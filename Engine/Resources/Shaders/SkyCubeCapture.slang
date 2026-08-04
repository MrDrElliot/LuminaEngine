#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/Sky.slang"

// Bakes the procedural sky into a 6-face cubemap for the IBL passes; one thread per texel computes
// its world-space ray from (face, texel), camera-independent. No pass-local set: the cube is written
// via a bindless 2D-array UAV and the env params via a device-address pointer.

struct FPushConstants
{
    uint64_t Env;         // FEnvironmentParams, see LoadBlock
    uint   SkyCubeUAV;     // bindless 2D-array UAV of the sky cube (GetMipUAVIndex(0))
    float  Time;
    float3 SunDirection;
    float  _Pad;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define SKYCAP_TILE 8

[shader("compute")]
[numthreads(SKYCAP_TILE, SKYCAP_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    FEnvironmentParams uEnvironment = LoadBlock<FEnvironmentParams>(Pass().Env);
    RWTexture2DArray<float4> uSkyCube = gRWTextures2DArray[Pass().SkyCubeUAV];

    uint Width, Height, Layers;
    uSkyCube.GetDimensions(Width, Height, Layers);

    if (DTid.x >= Width || DTid.y >= Height || DTid.z >= Layers)
    {
        return;
    }

    // Texel-centered face UV in [0, 1]. Sky.slang::CubeFaceDirection remaps
    // to [-1, 1] internally for the face-axis math.
    float2 UV = (float2(DTid.xy) + 0.5) / float2(Width, Height);

    float3 RayDir = CubeFaceDirection(DTid.z, UV);
    float3 Color  = EvaluateSky(uEnvironment, Pass().SunDirection, RayDir, Pass().Time);

    uSkyCube[DTid] = float4(Color, 1.0);
}
