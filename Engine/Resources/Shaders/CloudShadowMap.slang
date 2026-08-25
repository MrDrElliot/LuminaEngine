#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/CloudCommon.slang"

// Top-down sun transmittance through the cloud layer, so fog below it is shaded by the weather overhead.

struct FPushConstants
{
    FCloudMedium Medium;

    uint   ShadowUAV;      // bindless 2D UAV, r = sun transmittance
    uint   Resolution;
    uint   MarchSteps;
    float  HalfExtent;     // world half-size of the square the map covers

    float2 Center;         // world XZ the map is centered on
    float  Absorption;     // extinction per unit of cloud density along the sun ray
    float  _Pad0;

    float3 SunDirection;   // surface toward the sun
    float  _Pad1;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DispatchID : SV_DispatchThreadID)
{
    uint Res = Pass().Resolution;
    if (DispatchID.x >= Res || DispatchID.y >= Res)
    {
        return;
    }

    FCloudMedium M = Pass().Medium;

    float2 UV      = (float2(DispatchID.xy) + 0.5) / float(Res);
    float2 WorldXZ = Pass().Center + (UV - 0.5) * (2.0 * Pass().HalfExtent);

    float3 SunDir = normalize(Pass().SunDirection);

    // A sun at or below the horizon casts no usable layer shadow, and the ray would never exit the slab.
    if (SunDir.y < 0.05)
    {
        gRWTextures2D[Pass().ShadowUAV][int2(DispatchID.xy)] = float4(1.0, 0.0, 0.0, 0.0);
        return;
    }

    // March the slab from where the ray enters it to where it leaves, both along the sun ray.
    float3 Origin = float3(WorldXZ.x, M.LayerBottom, WorldXZ.y);
    float  Span   = (M.LayerTop - M.LayerBottom) / SunDir.y;

    uint  Steps = max(Pass().MarchSteps, 1u);
    float Step  = Span / float(Steps);

    float OpticalDepth = 0.0;
    for (uint i = 0; i < Steps; ++i)
    {
        float3 P = Origin + SunDir * (Step * (float(i) + 0.5));
        OpticalDepth += Cloud_SampleDensityM(M, P, Cloud_HeightFraction(M, P.y), true) * Step;
    }

    float Transmittance = exp(-OpticalDepth * Pass().Absorption);
    gRWTextures2D[Pass().ShadowUAV][int2(DispatchID.xy)] = float4(Transmittance, 0.0, 0.0, 0.0);
}
