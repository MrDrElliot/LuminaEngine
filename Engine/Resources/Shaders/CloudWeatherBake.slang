#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/CloudCommon.slang"

struct FPushConstants
{
    FTexture3D   Noise;
    FRWTexture2D Weather;
    uint         Size;
    uint         _Pad0;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy >= uint2(Pass().Size, Pass().Size)))
    {
        return;
    }
    float2 UV = (float2(DTid.xy) + 0.5) / float(Pass().Size);
    Pass().Weather[int2(DTid.xy)] = Pass().Noise.SampleLevel(Samplers::LinearWrap, float3(UV.x, kWeatherSliceV, UV.y), 0.0);
}
