// No rasterizer in a compute entry point, so nothing reached from here may use ddx/ddy.
#define SHADING_COMPUTE 1

#include "Includes/SceneGlobals.slang"
#include "Includes/Fog.slang"
#include "Includes/Froxel.slang"
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
    float FarShaftDistance;

    uint  FogShaftIndex;             // kAtmosphereTermOff keeps the far field closed-form and unshadowed
    uint  FogShaftDepthIndex;        // raw depth each half-res shaft texel was marched against
    uint  CloudDepthIndex;           // raw depth each half-res cloud texel was marched against
    float CloudFloor;                // altitude of the cloud layer's base
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Every term is a per-channel scale and offset of the HDR color, so they fold into one before HDR is touched.
struct FHDRTerm
{
    float3 Scale;
    float3 Offset;
};

void ComposeTerm(inout FHDRTerm Total, float3 Scale, float3 Offset)
{
    Total.Scale  *= Scale;
    Total.Offset  = Total.Offset * Scale + Offset;
}

FHDRTerm AerialPerspectiveTerm(float2 UV, float Depth)
{
    float3 ViewPos  = ReconstructViewPos(UV, Depth, GetInverseCameraProjection());
    float3 WorldPos = mul(GetInverseCameraView(), float4(ViewPos, 1.0)).xyz;

    // Inverse of the LUT's squared slice spacing; a linear lookup here reads the wrong slice.
    float RayDist = length(WorldPos - GetCameraPosition());
    float W       = saturate(sqrt(RayDist / max(Pass().AerialRange, 1.0)));

    float3 UVW       = float3(UV, W);
    // Explicit level, since an implicit one needs compute derivatives and the LUT has a single mip anyway.
    float3 InScatter = SampleTexture3DLevel(Pass().AerialInScatterIndex,     SAMPLER_LINEAR_CLAMP, UVW, 0.0).rgb;
    float3 Trans     = SampleTexture3DLevel(Pass().AerialTransmittanceIndex, SAMPLER_LINEAR_CLAMP, UVW, 0.0).rgb;

    const float Intensity = saturate(Pass().AerialIntensity);
    FHDRTerm Term;
    Term.Scale  = lerp(float3(1.0), Trans, Intensity);
    Term.Offset = InScatter * Intensity;
    return Term;
}

// Returns in-scatter in rgb and the clamped fog opacity in a, the pair the old blend state consumed.
// A texel marched against a nearer surface stopped short, so blending it into a pixel behind that surface leaves a clear halo.
static const float kCloudDepthMismatch  = 1.0e6;
static const float kCloudDepthSharpness = 32.0;

// In half-res texels; 1 would be plain bilinear.
static const float kCloudTentRadius = 1.6;

// How far a cloud texel's marched depth is from this pixel's, relative to the pixel's own distance.
float CloudDepthError(float PixelRaw, float PixelLinear, float TexelRaw)
{
    bool bPixelSky = PixelRaw <= 0.0;
    bool bTexelSky = TexelRaw <= 0.0;
    if (bPixelSky && bTexelSky)
    {
        return 0.0;
    }
    if (bPixelSky)
    {
        // Among texels that all stopped at geometry, the farthest carries the most of the sky's cloud.
        return kCloudDepthMismatch + 1.0 / max(LinearizeSceneDepth(TexelRaw), 1e-3);
    }
    if (bTexelSky)
    {
        return kCloudDepthMismatch;
    }
    return abs(LinearizeSceneDepth(TexelRaw) - PixelLinear) / PixelLinear;
}

// Bilinear over the four nearest half-res texels, weighted toward those marched against this pixel's depth.
float4 UpsampleClouds(float2 UV, float Depth)
{
    uint Width;
    uint Height;
    gTextures2D[Pass().CloudScatterIndex].GetDimensions(Width, Height);
    int2 MaxTexel = int2(Width, Height) - 1;

    float2 Coord = UV * float2(Width, Height) - 0.5;
    int2   Base  = int2(floor(Coord));
    float2 Frac  = Coord - float2(Base);

    float PixelLinear = Depth > 0.0 ? LinearizeSceneDepth(Depth) : 0.0;

    float4 Sum        = float4(0.0, 0.0, 0.0, 0.0);
    float  WeightSum  = 0.0;
    float4 Closest    = float4(0.0, 0.0, 0.0, 1.0);
    float  ClosestErr = 3.0e38;

    // A tent wider than bilinear, so the march's per-pixel start jitter averages out on thin wisps.
    [unroll]
    for (int i = 0; i < 16; ++i)
    {
        int2   Offset = int2(i & 3, i >> 2) - 1;
        int2   Texel  = clamp(Base + Offset, int2(0, 0), MaxTexel);
        float4 Cloud  = gTextures2D[Pass().CloudScatterIndex].Load(int3(Texel, 0));
        float  Err    = CloudDepthError(Depth, PixelLinear, gTextures2D[Pass().CloudDepthIndex].Load(int3(Texel, 0)).r);

        float2 Axis   = max(kCloudTentRadius - abs(float2(Offset) - Frac), 0.0);
        float  Weight = Axis.x * Axis.y / (1.0 + Err * kCloudDepthSharpness);
        Sum          += Cloud * Weight;
        WeightSum    += Weight;

        if (Err < ClosestErr)
        {
            ClosestErr = Err;
            Closest    = Cloud;
        }
    }

    // No texel matched, as in a sky gap narrower than a texel, so the best match is taken alone.
    return WeightSum > 1.0e-4 ? Sum / WeightSum : Closest;
}

// The four nearest half-res shaft texels, weighted toward those marched against this pixel's depth.
float4 UpsampleFogShafts(float2 UV, float Depth)
{
    uint Width;
    uint Height;
    gTextures2D[Pass().FogShaftIndex].GetDimensions(Width, Height);
    const int2 MaxTexel = int2(Width, Height) - 1;

    const float2 Coord = UV * float2(Width, Height) - 0.5;
    const int2   Base  = int2(floor(Coord));
    const float2 Frac  = Coord - float2(Base);

    const float PixelLinear = Depth > 0.0 ? LinearizeSceneDepth(Depth) : 0.0;

    float4 Sum        = float4(0.0, 0.0, 0.0, 0.0);
    float  WeightSum  = 0.0;
    float4 Closest    = float4(0.0, 0.0, 0.0, 1.0);
    float  ClosestErr = 3.0e38;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const int2   Offset = int2(i & 1, i >> 1);
        const int2   Texel  = clamp(Base + Offset, int2(0, 0), MaxTexel);
        const float4 Shaft  = gTextures2D[Pass().FogShaftIndex].Load(int3(Texel, 0));
        const float  Err    = CloudDepthError(Depth, PixelLinear, gTextures2D[Pass().FogShaftDepthIndex].Load(int3(Texel, 0)).r);

        const float2 Axis   = lerp(1.0 - Frac, Frac, float2(Offset));
        const float  Weight = Axis.x * Axis.y / (1.0 + Err * kCloudDepthSharpness);
        Sum       += Shaft * Weight;
        WeightSum += Weight;

        if (Err < ClosestErr)
        {
            ClosestErr = Err;
            Closest    = Shaft;
        }
    }

    return WeightSum > 1.0e-4 ? Sum / WeightSum : Closest;
}

// Each term is frame-uniform, so a frame without it specializes to 0 and sheds that term's registers.
[vk::constant_id(13)] const uint SPEC_ATMOS_AERIAL     = 1;
[vk::constant_id(14)] const uint SPEC_ATMOS_CLOUDS     = 1;
[vk::constant_id(15)] const uint SPEC_ATMOS_FOG        = 1;
[vk::constant_id(16)] const uint SPEC_ATMOS_VOLUMETRIC = 1;
[vk::constant_id(17)] const uint SPEC_ATMOS_SHAFTS     = 1;

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
    if (SPEC_ATMOS_VOLUMETRIC != 0 && Pass().bVolumetric != 0)
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

    // Marched at half resolution by FogShafts, because the closed form cannot carry a shadow term.
    float TMarchEnd = T0;
    if (SPEC_ATMOS_SHAFTS != 0 && bSunVolumetric && Pass().FogShaftIndex != kAtmosphereTermOff)
    {
        const float Reach = T0 + Pass().FarShaftDistance;
        TMarchEnd = bSky ? Reach : min(T1, Reach);

        if (TMarchEnd > T0)
        {
            const float4 Shaft = UpsampleFogShafts(UV, Depth);
            InScatter += Trans * Shaft.rgb;
            Trans     *= Shaft.a;
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

        float3 Light = GetVolumetricAmbientRadiance();
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

    float Depth = gTextures2D[Pass().DepthIndex].Load(int3(P, 0)).r;

    FHDRTerm Total;
    Total.Scale  = float3(1.0);
    Total.Offset = float3(0.0);

    // Sky already carries the full atmosphere integral, and reverse-Z leaves it at depth 0.
    if (SPEC_ATMOS_AERIAL != 0 && Pass().AerialInScatterIndex != kAtmosphereTermOff && Depth > 0.0)
    {
        const FHDRTerm Aerial = AerialPerspectiveTerm(UV, Depth);
        ComposeTerm(Total, Aerial.Scale, Aerial.Offset);
    }

    // A ray climbs at most its own length, so a surface nearer than the gap to the cloud base has no cloud in front.
    const float PlanetCurvatureMargin = 0.99;
    const float CloudGap       = max(Pass().CloudFloor - GetCameraPosition().y, 0.0) * PlanetCurvatureMargin;
    const bool  bCloudReachable = Depth <= 0.0
        || length(ReconstructViewPos(UV, Depth, GetInverseCameraProjection())) >= CloudGap;
    if (SPEC_ATMOS_CLOUDS != 0 && Pass().CloudScatterIndex != kAtmosphereTermOff && bCloudReachable)
    {
        float4 Cloud = UpsampleClouds(UV, Depth);
        if (Cloud.a <= 0.999)
        {
            ComposeTerm(Total, float3(Cloud.a), Cloud.rgb);
        }
    }

    if (SPEC_ATMOS_FOG != 0 && Pass().bFog != 0)
    {
        float4 Fog = IntegrateFog(UV, PixelPos, Depth);
        ComposeTerm(Total, float3(1.0 - Fog.a), Fog.rgb);
    }

    // HDR is only read and written where some term changes it.
    if (any(Total.Scale != 1.0) || any(Total.Offset != 0.0))
    {
        const float4 Color = gRWTextures2D[Pass().HDRUAV][P];
        gRWTextures2D[Pass().HDRUAV][P] = float4(Color.rgb * Total.Scale + Total.Offset, Color.a);
    }
}
