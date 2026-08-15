#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"

// Upsamples the half-res cloud march onto HDR; clouds are low frequency so a bilinear tap suffices.

struct FPushConstants
{
    uint  HDRUAV;        // bindless 2D UAV: HDR, composited in place
    uint  ScatterIndex;  // bindless 2D SRV: half-res rgb = radiance, a = transmittance
    uint  ScreenW;
    uint  ScreenH;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    uint2 Size = uint2(Pass().ScreenW, Pass().ScreenH);
    if (DTid.x >= Size.x || DTid.y >= Size.y)
    {
        return;
    }

    int2   P  = int2(DTid.xy);
    float2 UV = (float2(DTid.xy) + 0.5) / float2(Size);

    float4 Cloud = SampleTexture2DLevel(Pass().ScatterIndex, SAMPLER_LINEAR_CLAMP, UV, 0.0);
    if (Cloud.a > 0.999)
    {
        return;
    }

    float4 Dst = gRWTextures2D[Pass().HDRUAV][P];
    gRWTextures2D[Pass().HDRUAV][P] = float4(Dst.rgb * Cloud.a + Cloud.rgb, Dst.a);
}
