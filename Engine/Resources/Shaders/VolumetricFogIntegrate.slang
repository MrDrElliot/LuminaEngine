#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/Froxel.slang"

// Froxel volumetric fog integrate pass: one thread per (x,y) column marches Z front-to-back,
// accumulating transmittance-weighted in-scatter and writing (accum rgb, transmittance) per froxel.
// No pass-local set: the scatter volume (3D SRV) and integrated volume (3D UAV) are bindless.

struct FPushConstants
{
    uint3 GridSize;
    float NearPlane;
    float FogRange;
    uint  ScatterSRV;     // bindless 3D SRV: scatter volume (in-scatter rgb, extinction)
    uint  IntegratedUAV;  // bindless 3D UAV: integrated volume (accum rgb, transmittance)
    uint  _Pad0;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DispatchID : SV_DispatchThreadID)
{
    if (DispatchID.x >= Pass().GridSize.x || DispatchID.y >= Pass().GridSize.y)
    {
        return;
    }

    float3 Accum = float3(0.0);
    float  Trans = 1.0;

    // Adjacent slices share a boundary: DFar(z) == DNear(z+1). Carry it forward so the exponential
    // slice distribution costs one pow per slice instead of two. DNear(0) = NearPlane (pow(.,0) == 1).
    float DNear = Pass().NearPlane;

    for (uint z = 0; z < Pass().GridSize.z; ++z)
    {
        float4 S      = gTextures3D[Pass().ScatterSRV].Load(int4(int(DispatchID.x), int(DispatchID.y), int(z), 0));
        float  SigmaT = max(S.a, 0.0);

        float DFar = Froxel_SliceDistance(float(z + 1), float(Pass().GridSize.z), Pass().NearPlane, Pass().FogRange);
        float Dz   = max(DFar - DNear, 0.0);

        float  SliceTrans = exp(-SigmaT * Dz);
        // Analytic integral of in-scatter across the slice (energy-conserving).
        float3 SliceInt   = (SigmaT > 1e-5) ? (S.rgb - S.rgb * SliceTrans) / SigmaT : S.rgb * Dz;

        Accum += Trans * SliceInt;
        Trans *= SliceTrans;

        gRWTextures3D[Pass().IntegratedUAV][int3(int(DispatchID.x), int(DispatchID.y), int(z))] = float4(Accum, Trans);

        DNear = DFar;
    }
}
