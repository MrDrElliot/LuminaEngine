#include "Includes/SceneGlobals.slang"
#include "Includes/Fog.slang"
#include "Includes/Froxel.slang"

// Unified fog composite: the shadowed froxel volume covers the near range (when volumetrics are
// on), then the same exp-height medium continues analytically to the scene depth -- or to infinity
// for sky pixels, which is what ties distant terrain and the horizon together. Both halves share
// one density profile, albedo, and lighting model (ambient + phase-weighted sun, unshadowed in the
// far field), so the hand-off at the froxel far plane is continuous. Composited via
// (One, InvSrcAlpha) blend (alpha = opacity), so the scene HDR is never read back.

struct FPushConstants
{
    uint64_t Fog;         // FExponentialHeightFogParams, see LoadBlock
    uint  DepthIndex;        // bindless 2D SRV: scene depth
    uint  IntegratedIndex;   // bindless 3D SRV: integrated froxel volume
    uint  GridZ;
    float NearPlane;
    float FogRange;
    uint  bVolumetric;       // froxel volume valid this frame (volumetrics enabled)
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
    return frac(52.9829189 * frac(dot(PixelPos, float2(0.06711056, 0.00583715))));
}

[shader("fragment")]
float4 FragmentMain(PSInput Input) : SV_Target0
{
    FExponentialHeightFogParams uFog = LoadBlock<FExponentialHeightFogParams>(Pass().Fog);

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

    // 2) Analytic segment: from the froxel far plane (or the camera + start distance when
    // volumetrics are off) to the surface, or to infinity for sky. Same medium, unshadowed.
    float VolEndZ = Pass().bVolumetric != 0 ? Pass().FogRange : 0.0;
    float T0      = max(VolEndZ / ZPerT, uFog.HeightParams.z);      // ray-space segment start
    float T1      = bSky ? 0.0 : ViewDist / ZPerT;                  // ray-space distance to surface

    if (bSky || T1 > T0)
    {
        float3 P0 = CamPos + RayDir * T0;
        float  OD = bSky ? Fog_OpticalDepthToInfinity(uFog, P0, RayDir)
                         : Fog_OpticalDepth(uFog, P0, RayDir, T1 - T0);
        float TransA = exp(-OD);

        // Same lighting model as the froxel inject with Shadow = 1: ambient skylight plus the
        // phase-weighted sun when it opted into volumetrics. Keeps the seam continuous and makes
        // fog track time of day automatically.
        float3 ToCam   = -RayDir;
        float3 Light   = GetAmbientLightColor() * GetAmbientLightIntensity();
        bool   bHasSun = GetNumLights() > 0 && HasFlag(GetLightAt(0).Flags, ELightFlags::Directional);
        float  CosSun  = -1.0;
        if (bHasSun)
        {
            FLight Sun = GetLightAt(0);
            // Sun.Direction points toward the sun; negate for the travel-direction phase
            // convention so CosSun = +1 when looking straight at the sun.
            CosSun = dot(-normalize(Sun.Direction), ToCam);
            if (HasFlag(Sun.Flags, ELightFlags::Volumetric))
            {
                Light += UnpackColor(Sun.Color).rgb * Sun.Intensity
                       * Fog_Phase(CosSun, uFog.VolumetricParams.y) * Sun.VolumetricIntensity;
            }
        }

        // Ramp the directional tint by the segment's mid distance; sky is at infinity (fully ramped).
        float3 Albedo = Fog_Albedo(uFog, CosSun, bSky ? 1e9 : 0.5 * (T0 + T1));
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
