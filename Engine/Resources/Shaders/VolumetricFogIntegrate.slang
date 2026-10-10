#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Froxel.slang"

// One thread per column marches Z front to back, writing (accum rgb, transmittance) per froxel.

struct FPushConstants
{
    uint3        GridSize;
    float        NearPlane;
    float        FogRange;
    FTexture3D   Scatter;      // scatter volume (in-scatter rgb, extinction)
    FRWTexture3D Integrated;   // integrated volume (accum rgb, transmittance)
    uint         _Pad0;
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

    // Adjacent slices share a boundary, so carrying DFar forward costs one pow per slice instead of two.
    float DNear = Pass().NearPlane;

    for (uint z = 0; z < Pass().GridSize.z; ++z)
    {
        float4 S      = Pass().Scatter.Load(int4(int(DispatchID.x), int(DispatchID.y), int(z), 0));
        float  SigmaT = max(S.a, 0.0);

        float DFar = Froxel_SliceDistance(float(z + 1), float(Pass().GridSize.z), Pass().NearPlane, Pass().FogRange);
        float Dz   = max(DFar - DNear, 0.0);

        float  SliceTrans = exp(-SigmaT * Dz);
        // Analytic integral of in-scatter across the slice, which conserves energy.
        float3 SliceInt   = (SigmaT > 1e-5) ? (S.rgb - S.rgb * SliceTrans) / SigmaT : S.rgb * Dz;

        Accum += Trans * SliceInt;
        Trans *= SliceTrans;

        Pass().Integrated[int3(int(DispatchID.x), int(DispatchID.y), int(z))] = float4(Accum, Trans);

        DNear = DFar;
    }
}
