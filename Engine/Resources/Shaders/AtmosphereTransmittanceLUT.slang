#include "Includes/SceneGlobals.slang"
#include "Includes/Sky.slang"

// Matches FTransmittanceLUTArgs in SceneRenderer_Environment.cpp.
struct FTransmittanceLUTArgs
{
    FRWTexture2D Out;
    float        Aerosol;
    uint         _Pad0;
    uint         _Pad1;
};
FTransmittanceLUTArgs* Pass() { return GetPassArgs<FTransmittanceLUTArgs>(); }

static const int kTransmittanceSteps = 40;

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= SKY_TRANSMITTANCE_LUT_WIDTH || DTid.y >= SKY_TRANSMITTANCE_LUT_HEIGHT)
    {
        return;
    }

    float Radius;
    float CosZenith;
    Sky_TransmittanceParams(float2(DTid.xy) / (kTransmittanceLUTSize - 1.0), Radius, CosZenith);

    float3 Origin = float3(0.0, Radius, 0.0);
    float3 Dir    = float3(sqrt(saturate(1.0 - CosZenith * CosZenith)), CosZenith, 0.0);
    float  Dt     = max(Sky_RaySphereIntersect(Origin, Dir, kAtmosphereR).y, 0.0) / float(kTransmittanceSteps);

    float3 OpticalDepth = float3(0.0, 0.0, 0.0);
    [[loop]]
    for (int i = 0; i < kTransmittanceSteps; ++i)
    {
        float3 P = Origin + Dir * ((float(i) + 0.5) * Dt);
        OpticalDepth += Sky_Medium(max(length(P) - kPlanetR, 0.0), Pass().Aerosol).Extinction * Dt;
    }

    Pass().Out[DTid.xy] = float4(exp(-OpticalDepth), 1.0);
}
