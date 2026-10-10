// No rasterizer in a compute entry point, so nothing reached from here may use ddx/ddy.
#define SHADING_COMPUTE 1

#include "Includes/SceneGlobals.slang"
#include "Includes/Fog.slang"
#include "Includes/ShadowSampling.slang"
#include "Includes/CloudCommon.slang"
#include "Includes/GlobalRHI.slang"

// The sun-shadowed far-field fog segment past the froxel volume, marched at half resolution for AtmosphereComposite.

struct FPushConstants
{
    FTexture2D   Depth;
    FRWTexture2D Shaft;          // rgb in-scatter, a transmittance of the marched segment
    FRWTexture2D ShaftDepth;     // raw depth each texel marched against, 0 where it saw sky
    uint         ScreenW;

    uint  ScreenH;
    uint  TraceW;
    uint  TraceH;
    uint  Steps;

    float      FogRange;
    float      ShaftDistance;
    uint       bVolumetric;
    FTexture2D CloudShadow;  // Invalid when no cloud shadow was built.

    float CloudShadowExtent;
    float CloudShadowCenterX;
    float CloudShadowCenterY;
    uint  _Pad0;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

float4 MarchShafts(float2 UV, float2 TexelPos, float Depth)
{
    FExponentialHeightFogParams uFog = Scene().FogParams;

    float3 CamPos  = GetCameraPosition();
    float2 NDC     = UV * 2.0 - 1.0;
    float4 ViewH   = mul(GetInverseCameraProjection(), float4(NDC, 1.0, 1.0));
    float3 ViewRay = ViewH.xyz / ViewH.w;
    float3 RayDir  = normalize(mul(GetInverseCameraView(), float4(ViewRay, 0.0)).xyz);
    float  ZPerT   = max(ViewRay.z, 1e-4) / length(ViewRay);

    const bool bHasSun = GetNumLights() > 0 && HasFlag(GetLightAt(0).Flags, ELightFlags::Directional);
    const FLight Sun   = GetLightAt(0);
    if (!bHasSun || !HasFlag(Sun.Flags, ELightFlags::Volumetric))
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const bool  bSky     = Depth <= 0.0;
    const float ViewDist = bSky ? Scene().FarPlane : ReconstructViewPos(UV, Depth, GetInverseCameraProjection()).z;

    const float T0    = (Pass().bVolumetric != 0 ? Pass().FogRange : 0.0) / ZPerT;
    const float T1    = bSky ? 0.0 : ViewDist / ZPerT;
    const float Reach = T0 + Pass().ShaftDistance;
    const float TEnd  = bSky ? Reach : min(T1, Reach);
    if (TEnd <= T0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    // Sun.Direction points toward the sun, so negate it for the travel-direction phase convention.
    const float  CosSun      = dot(-normalize(Sun.Direction), -RayDir);
    const float3 SunRadiance = UnpackColor(Sun.Color).rgb * Sun.Intensity * Sun.VolumetricIntensity;

    const float  StepLen      = (TEnd - T0) / float(Pass().Steps);
    // Same per-pixel hash the froxel lookup uses, so step banding lands as grain, not arcs.
    const float  Jitter       = InterleavedGradientNoise(TexelPos);
    const float2 ShadowCenter = float2(Pass().CloudShadowCenterX, Pass().CloudShadowCenterY);
    const float3 Ambient      = GetVolumetricAmbientRadiance();
    // The march only recedes, so each step resumes the cascade search where the last one hit.
    int CascadeCursor = 0;

    float3 InScatter = float3(0.0);
    float  Trans     = 1.0;
    for (uint i = 0u; i < Pass().Steps; ++i)
    {
        // Later steps all scale by Trans, so below this nothing further can still be seen.
        if (Trans < 1.0e-6)
        {
            break;
        }

        float  T = T0 + StepLen * (float(i) + Jitter);
        float3 P = CamPos + RayDir * T;

        float Density = Fog_DensityAt(uFog, P) * Fog_StartRamp(uFog, T);
        if (Density <= 0.0)
        {
            continue;
        }

        float Shadow = SampleSunCascadeFrom(Sun, P, CascadeCursor)
                     * CloudShadow_Sample(Pass().CloudShadow, P, ShadowCenter, Pass().CloudShadowExtent);

        float3 Light  = Ambient + SunRadiance * Fog_MultiScatterPhaseShadow(uFog, CosSun, uFog.VolumetricParams.y, Shadow);
        float3 Albedo = Fog_Albedo(uFog, CosSun, T);

        float  StepTrans = exp(-Density * StepLen);
        // Analytic integral of in-scatter across the step, matching the froxel integrate pass.
        InScatter += Trans * Albedo * Light * uFog.VolumetricParams.x * (1.0 - StepTrans);
        Trans     *= StepTrans;
    }
    return float4(InScatter, Trans);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= Pass().TraceW || DTid.y >= Pass().TraceH)
    {
        return;
    }

    const int2 P       = int2(DTid.xy);
    const int2 MaxFull = int2(Pass().ScreenW, Pass().ScreenH) - 1;
    const int2 Full    = P * 2;

    // The nearest of the four covered pixels, as the cloud march does, so a near edge never marches past its surface.
    const float D0 = Pass().Depth.Load(int3(min(Full,               MaxFull), 0)).r;
    const float D1 = Pass().Depth.Load(int3(min(Full + int2(1, 0), MaxFull), 0)).r;
    const float D2 = Pass().Depth.Load(int3(min(Full + int2(0, 1), MaxFull), 0)).r;
    const float D3 = Pass().Depth.Load(int3(min(Full + int2(1, 1), MaxFull), 0)).r;
    const float Depth = max(max(D0, D1), max(D2, D3));

    const float2 UV = (float2(Full) + 1.0) / float2(Pass().ScreenW, Pass().ScreenH);

    Pass().ShaftDepth[P] = float4(Depth, 0.0, 0.0, 0.0);
    Pass().Shaft[P]      = MarchShafts(UV, float2(P) + 0.5, Depth);
}
