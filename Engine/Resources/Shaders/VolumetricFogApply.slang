#include "Includes/SceneGlobals.slang"
#include "Includes/Fog.slang"
#include "Includes/Froxel.slang"
#include "Includes/ShadowSampling.slang"
#include "Includes/CloudCommon.slang"

// Froxel volume, then a shadowed march, then the closed form, all on one medium so hand-offs match.

struct FPushConstants
{
    uint  DepthIndex;        // bindless 2D SRV: scene depth
    uint  IntegratedIndex;   // bindless 3D SRV: integrated froxel volume
    uint  GridZ;
    float NearPlane;

    float FogRange;
    uint  bVolumetric;       // froxel volume valid this frame (volumetrics enabled)
    uint  FarShaftSteps;     // 0 = far field stays closed-form and unshadowed
    float FarShaftDistance;

    uint  CloudShadowIndex;  // bindless 2D SRV, 0xFFFFFFFF when no cloud shadow was built
    float CloudShadowExtent;
    float2 CloudShadowCenter;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

struct PSInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

// Interleaved gradient noise: stable per-pixel hash used to dither the depth sample by < one slice,
// breaking exponential-Z banding into fine full-resolution grain.
float IGN(float2 PixelPos)
{
    return InterleavedGradientNoise(PixelPos);
}

[shader("fragment")]
float4 FragmentMain(PSInput Input) : SV_Target0
{
    FExponentialHeightFogParams uFog = Scene().FogParams;

    float2 UV    = Input.UV;
    float  Depth = gTextures2D[Pass().DepthIndex].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], UV, 0.0).r;

    // World ray through this pixel. Reverse-Z: Depth == 0 is the far plane (sky).
    // View space is +Z forward, so visible rays have ViewRay.z > 0.
    float3 CamPos = GetCameraPosition();
    float2 NDC    = UV * 2.0 - 1.0;
    float4 ViewH  = mul(GetInverseCameraProjection(), float4(NDC, 1.0, 1.0));
    float3 ViewRay = ViewH.xyz / ViewH.w;                           // point on the pixel ray in view space
    float3 RayDir  = normalize(mul(GetInverseCameraView(), float4(ViewRay, 0.0)).xyz);
    // View-Z per unit of ray length: converts view-Z plane distances (froxel slicing) to ray params.
    float  ZPerT   = max(ViewRay.z, 1e-4) / length(ViewRay);

    bool  bSky = Depth <= 0.0;
    float ViewDist;                                                 // view-Z distance to the surface
    if (!bSky)
    {
        ViewDist = ReconstructViewPos(UV, Depth, GetInverseCameraProjection()).z;
    }
    else
    {
        ViewDist = Scene().FarPlane;
    }

    // 1) Froxel segment: camera -> min(surface, froxel range), shadowed in-scatter from the volume.
    float3 InScatter = float3(0.0);
    float  Trans     = 1.0;
    if (Pass().bVolumetric != 0)
    {
        float W = bSky ? 1.0 : Froxel_DistanceToW(ViewDist, Pass().NearPlane, Pass().FogRange);
        // Dither the depth sample by < half a slice to break exponential-Z banding into
        // fine per-pixel grain instead of visible color steps.
        W = saturate(W + (IGN(Input.Position.xy) - 0.5) / float(Pass().GridZ));

        float4 Froxel = gTextures3D[Pass().IntegratedIndex].SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], float3(UV, W), 0.0);
        InScatter = Froxel.rgb;
        Trans     = saturate(Froxel.a);
    }

    // Shared lighting for the far field, so its hand-off with the froxel volume stays continuous.
    float3 ToCam   = -RayDir;
    float3 Ambient = GetVolumetricAmbientRadiance();
    bool   bHasSun = GetNumLights() > 0 && HasFlag(GetLightAt(0).Flags, ELightFlags::Directional);
    FLight Sun     = GetLightAt(0);
    float  CosSun  = -1.0;
    bool   bSunVolumetric = false;
    if (bHasSun)
    {
        // Sun.Direction points toward the sun, so negate it for the travel-direction phase convention.
        CosSun = dot(-normalize(Sun.Direction), ToCam);
        bSunVolumetric = HasFlag(Sun.Flags, ELightFlags::Volumetric);
    }
    float3 SunRadiance = bSunVolumetric ? UnpackColor(Sun.Color).rgb * Sun.Intensity * Sun.VolumetricIntensity
                                        : float3(0.0);

    // Far field runs from the froxel far plane to the surface, or to infinity for sky.
    float VolEndZ = Pass().bVolumetric != 0 ? Pass().FogRange : 0.0;
    float T0      = VolEndZ / ZPerT;
    float T1      = bSky ? 0.0 : ViewDist / ZPerT;

    // Marched because the closed form cannot carry a shadow term, and shafts would stop at the volume.
    uint MarchSteps = (bSunVolumetric && Pass().FarShaftSteps > 0u) ? Pass().FarShaftSteps : 0u;
    float TMarchEnd = T0;
    if (MarchSteps != 0u)
    {
        float Reach = T0 + Pass().FarShaftDistance;
        TMarchEnd   = bSky ? Reach : min(T1, Reach);

        if (TMarchEnd > T0)
        {
            float StepLen  = (TMarchEnd - T0) / float(MarchSteps);
            // Same per-pixel hash the froxel lookup uses, so step banding lands as grain, not arcs.
            float Jitter   = IGN(Input.Position.xy);

            for (uint i = 0u; i < MarchSteps; ++i)
            {
                float  T = T0 + StepLen * (float(i) + Jitter);
                float3 P = CamPos + RayDir * T;

                float Density = Fog_DensityAt(uFog, P) * Fog_StartRamp(uFog, T);
                if (Density <= 0.0)
                {
                    continue;
                }

                float Shadow = SampleSunCascadeByContainment(Sun, P)
                             * CloudShadow_Sample(Pass().CloudShadowIndex, P, Pass().CloudShadowCenter, Pass().CloudShadowExtent);

                float3 Light = Ambient + SunRadiance * Fog_MultiScatterPhaseShadow(uFog, CosSun, uFog.VolumetricParams.y, Shadow);
                float3 Albedo = Fog_Albedo(uFog, CosSun, T);

                float  StepTrans = exp(-Density * StepLen);
                // Analytic integral of in-scatter across the step, matching the froxel integrate pass.
                float3 StepInt   = Albedo * Light * uFog.VolumetricParams.x * (1.0 - StepTrans);

                InScatter += Trans * StepInt;
                Trans     *= StepTrans;
            }
        }
    }

    // Shadow = 1 makes the octave shadow-leak term identity, which is what the march converges to.
    float TRest = max(TMarchEnd, uFog.HeightParams.z);
    if (bSky || T1 > TRest)
    {
        float3 P0 = CamPos + RayDir * TRest;
        float  OD = bSky ? Fog_OpticalDepthToInfinity(uFog, P0, RayDir)
                         : Fog_OpticalDepth(uFog, P0, RayDir, T1 - TRest);
        // Ramped at the mid distance so the near half's soft front does not become a hard one here.
        OD *= Fog_StartRamp(uFog, bSky ? 1e9 : 0.5 * (TRest + T1));
        float TransA = exp(-OD);

        float3 Light = Ambient;
        if (bSunVolumetric)
        {
            Light += SunRadiance * Fog_MultiScatterPhaseShadow(uFog, CosSun, uFog.VolumetricParams.y, 1.0);
        }

        // Ramp the directional tint by the segment's mid distance; sky is at infinity (fully ramped).
        float3 Albedo = Fog_Albedo(uFog, CosSun, bSky ? 1e9 : 0.5 * (TRest + T1));
        InScatter += Trans * Albedo * Light * uFog.VolumetricParams.x * (1.0 - TransA);
        Trans     *= TransA;
    }

    // FogMaxOpacity caps the combined result; rescale the in-scatter so the clamp dims the fog
    // instead of just letting the scene bleed through over-bright haze.
    float Opacity = 1.0 - Trans;
    float Clamped = min(Opacity, uFog.HeightParams.w);
    InScatter    *= Opacity > 1e-4 ? Clamped / Opacity : 0.0;

    // (One, InvSrcAlpha): result = InScatter + HDR * (1 - Clamped).
    return float4(InScatter, Clamped);
}
