#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"

// In-place on HDR rather than a blended raster pass, so transmittance stays per-channel.

struct FPushConstants
{
    uint  HDRUAV;              // bindless 2D UAV: HDR scene colour, read-modify-write
    uint  DepthIndex;          // bindless 2D SRV: scene depth
    uint  InScatterIndex;      // bindless 3D SRV
    uint  TransmittanceIndex;  // bindless 3D SRV

    uint2 Size;
    float Range;               // must match the LUT build
    float Intensity;           // 0 = off, 1 = the physical result
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    uint2 Size = Pass().Size;
    if (DTid.x >= Size.x || DTid.y >= Size.y)
    {
        return;
    }

    int2  P     = int2(DTid.xy);
    float Depth = gTextures2D[Pass().DepthIndex].Load(int3(P, 0)).r;

    // Reverse-Z: 0 is the far plane, and the sky already carries the full atmosphere integral.
    if (Depth <= 0.0)
    {
        return;
    }

    float2 UV       = (float2(DTid.xy) + 0.5) / float2(Size);
    float3 ViewPos  = ReconstructViewPos(UV, Depth, GetInverseCameraProjection());
    float3 WorldPos = mul(GetInverseCameraView(), float4(ViewPos, 1.0)).xyz;

    float RayDist = length(WorldPos - GetCameraPosition());
    float W       = saturate(RayDist / max(Pass().Range, 1.0));

    float3 UVW       = float3(UV, W);
    float3 InScatter = SampleTexture3D(Pass().InScatterIndex,     SAMPLER_LINEAR_CLAMP, UVW).rgb;
    float3 Trans     = SampleTexture3D(Pass().TransmittanceIndex, SAMPLER_LINEAR_CLAMP, UVW).rgb;

    float4 Src    = gRWTextures2D[Pass().HDRUAV][P];
    float3 Fogged = Src.rgb * Trans + InScatter;

    gRWTextures2D[Pass().HDRUAV][P] = float4(lerp(Src.rgb, Fogged, saturate(Pass().Intensity)), Src.a);
}
