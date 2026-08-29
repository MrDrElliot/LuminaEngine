// No rasterizer in a compute entry point, so nothing reached from here may use ddx/ddy.
#define SHADING_COMPUTE 1

#include "Includes/SceneGlobals.slang"
#include "Includes/Fog.slang"
#include "Includes/Froxel.slang"
#include "Includes/ShadowSampling.slang"
#include "Includes/CloudCommon.slang"
#include "Includes/GlobalRHIStorage.slang"

// Aerial perspective, clouds and fog in one HDR read-modify-write, in the order the old passes ran.

static const uint kAtmosphereTermOff = 0xFFFFFFFFu;

struct FPushConstants
{
    uint  HDRUAV;
    uint  DepthIndex;
    uint  ScreenW;
    uint  ScreenH;

    uint  AerialInScatterIndex;      // kAtmosphereTermOff disables aerial perspective
    uint  AerialTransmittanceIndex;
    float AerialRange;               // must match the LUT build
    float AerialIntensity;

    uint  CloudScatterIndex;         // kAtmosphereTermOff disables clouds
    uint  bFog;
    uint  FroxelIntegratedIndex;
    uint  GridZ;

    float NearPlane;
    float FogRange;
    uint  bVolumetric;               // froxel volume valid this frame
    uint  FarShaftSteps;             // 0 = far field stays closed-form and unshadowed

    float FarShaftDistance;
    uint  CloudShadowIndex;          // kAtmosphereTermOff when no cloud shadow was built
    float CloudShadowExtent;
    float CloudShadowCenterX;

    float CloudShadowCenterY;
    float _Pad0;
    float _Pad1;
    float _Pad2;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

float3 ApplyAerialPerspective(float3 Color, float2 UV, float Depth)
{
    float3 ViewPos  = ReconstructViewPos(UV, Depth, GetInverseCameraProjection());
    float3 WorldPos = mul(GetInverseCameraView(), float4(ViewPos, 1.0)).xyz;

    // Inverse of the LUT's squared slice spacing; a linear lookup here reads the wrong slice.
    float RayDist = length(WorldPos - GetCameraPosition());
    float W       = saturate(sqrt(RayDist / max(Pass().AerialRange, 1.0)));

    float3 UVW       = float3(UV, W);
    float3 InScatter = SampleTexture3D(Pass().AerialInScatterIndex,     SAMPLER_LINEAR_CLAMP, UVW).rgb;
    float3 Trans     = SampleTexture3D(Pass().AerialTransmittanceIndex, SAMPLER_LINEAR_CLAMP, UVW).rgb;

    float3 Fogged = Color * Trans + InScatter;
    return lerp(Color, Fogged, saturate(Pass().AerialIntensity));
}

// Returns in-scatter in rgb and the clamped fog opacity in a, the pair the old blend state consumed.
float4 IntegrateFog(float2 UV, float2 PixelPos, float Depth)
{
    FExponentialHeightFogParams uFog = Scene().FogParams;

    // Reverse-Z puts the far plane at 0, and view space is +Z forward so visible rays have ViewRay.z > 0.
    float3 CamPos  = GetCameraPosition();
    float2 NDC     = UV * 2.0 - 1.0;
    float4 ViewH   = mul(GetInverseCameraProjection(), float4(NDC, 1.0, 1.0));
    float3 ViewRay = ViewH.xyz / ViewH.w;
    float3 RayDir  = normalize(mul(GetInverseCameraView(), float4(ViewRay, 0.0)).xyz);
    // View-Z per unit of ray length, converting the froxel slicing distances into ray parameters.
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

    // Froxel segment spans the camera to min(surface, froxel range), shadowed from the volume.
    float3 InScatter = float3(0.0);
    float  Trans     = 1.0;
    if (Pass().bVolumetric != 0)
    {
        float W = bSky ? 1.0 : Froxel_DistanceToW(ViewDist, Pass().NearPlane, Pass().FogRange);
        // Dithering the depth sample by under half a slice breaks exponential-Z banding into grain.
        W = saturate(W + (InterleavedGradientNoise(PixelPos) - 0.5) / float(Pass().GridZ));

        float4 Froxel = gTextures3D[Pass().FroxelIntegratedIndex].SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], float3(UV, W), 0.0);
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
    uint  MarchSteps = (bSunVolumetric && Pass().FarShaftSteps > 0u) ? Pass().FarShaftSteps : 0u;
    float TMarchEnd  = T0;
    if (MarchSteps != 0u)
    {
        float Reach = T0 + Pass().FarShaftDistance;
        TMarchEnd   = bSky ? Reach : min(T1, Reach);

        if (TMarchEnd > T0)
        {
            float  StepLen      = (TMarchEnd - T0) / float(MarchSteps);
            // Same per-pixel hash the froxel lookup uses, so step banding lands as grain, not arcs.
            float  Jitter       = InterleavedGradientNoise(PixelPos);
            float2 ShadowCenter = float2(Pass().CloudShadowCenterX, Pass().CloudShadowCenterY);
            // The march only recedes, so each step resumes the cascade search where the last one hit.
            int    CascadeCursor = 0;

            for (uint i = 0u; i < MarchSteps; ++i)
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
                             * CloudShadow_Sample(Pass().CloudShadowIndex, P, ShadowCenter, Pass().CloudShadowExtent);

                float3 Light  = Ambient + SunRadiance * Fog_MultiScatterPhaseShadow(uFog, CosSun, uFog.VolumetricParams.y, Shadow);
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

    // Rescale the in-scatter so FogMaxOpacity dims the fog instead of bleeding the scene through it.
    float Opacity = 1.0 - Trans;
    float Clamped = min(Opacity, uFog.HeightParams.w);
    InScatter    *= Opacity > 1e-4 ? Clamped / Opacity : 0.0;

    return float4(InScatter, Clamped);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    uint2 Size = uint2(Pass().ScreenW, Pass().ScreenH);
    if (DTid.x >= Size.x || DTid.y >= Size.y)
    {
        return;
    }

    int2   P        = int2(DTid.xy);
    float2 PixelPos = float2(DTid.xy) + 0.5;
    float2 UV       = PixelPos / float2(Size);

    float  Depth  = gTextures2D[Pass().DepthIndex].Load(int3(P, 0)).r;
    float4 Color  = gRWTextures2D[Pass().HDRUAV][P];
    float3 Result = Color.rgb;
    bool   bWrote = false;

    // Sky already carries the full atmosphere integral, and reverse-Z leaves it at depth 0.
    if (Pass().AerialInScatterIndex != kAtmosphereTermOff && Depth > 0.0)
    {
        Result = ApplyAerialPerspective(Result, UV, Depth);
        bWrote = true;
    }

    // Clouds are low frequency, so a bilinear tap off the half-res march suffices.
    if (Pass().CloudScatterIndex != kAtmosphereTermOff)
    {
        float4 Cloud = SampleTexture2DLevel(Pass().CloudScatterIndex, SAMPLER_LINEAR_CLAMP, UV, 0.0);
        if (Cloud.a <= 0.999)
        {
            Result = Result * Cloud.a + Cloud.rgb;
            bWrote = true;
        }
    }

    if (Pass().bFog != 0)
    {
        float4 Fog = IntegrateFog(UV, PixelPos, Depth);
        Result = Fog.rgb + Result * (1.0 - Fog.a);
        bWrote = true;
    }

    if (bWrote)
    {
        gRWTextures2D[Pass().HDRUAV][P] = float4(Result, Color.a);
    }
}
