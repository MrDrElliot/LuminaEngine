#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Sky.slang"

struct FPushConstants
{
    uint64_t          Env;            // FEnvironmentParams, see LoadBlock
    FRWTexture2DArray SkyCube;        // sky cube (GetMipUAVIndex(0))
    float             Time;
    float3            SunDirection;
    float             _Pad;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define SKYCAP_TILE 8

[shader("compute")]
[numthreads(SKYCAP_TILE, SKYCAP_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    FEnvironmentParams uEnvironment = LoadBlock<FEnvironmentParams>(Pass().Env);
    FRWTexture2DArray  uSkyCube = Pass().SkyCube;

    uint Width, Height, Layers;
    uSkyCube.GetDimensions(Width, Height, Layers);

    if (DTid.x >= Width || DTid.y >= Height || DTid.z >= Layers)
    {
        return;
    }

    float2 UV = (float2(DTid.xy) + 0.5) / float2(Width, Height);

    float3 RayDir = CubeFaceDirection(DTid.z, UV);
    float3 Color  = EvaluateSky(uEnvironment, Pass().SunDirection, RayDir, Pass().Time);

    uSkyCube[DTid] = float4(Color, 1.0);
}
