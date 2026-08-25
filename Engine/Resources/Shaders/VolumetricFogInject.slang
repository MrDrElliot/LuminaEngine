#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/ShadowSampling.slang"
#include "Includes/Fog.slang"
#include "Includes/Froxel.slang"
#include "Includes/CloudCommon.slang"

// Froxel volumetric fog inject pass: per froxel, write exp-height-fog extinction plus shadowed
// in-scatter from the sun + opted-in local lights into uFroxelScatter; the integrate pass accumulates.

struct FPushConstants
{
    uint3  GridSize;
    float  NearPlane;

    float  FogRange;
    uint   bSunVolumetric;
    float  Time;
    uint   ScatterUAV;          // bindless 3D UAV index of the froxel scatter volume

    uint   bSupersampleLocal;   // 1 = 4x supersample local light in-scatter per froxel
    uint   NumFogVolumes;
    uint   CloudShadowIndex;    // bindless 2D SRV, 0xFFFFFFFF when no cloud shadow was built
    float  CloudShadowExtent;

    float2 CloudShadowCenter;
    float2 _Pad0;

    // Pointer last so its 8-byte alignment lands on a multiple of 8.
    uint64_t FogVolumes;        // FFogVolume[NumFogVolumes] (transient device address)
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Froxel center -> world position. Slices are view-Z planes (exponential), matching
// the clustered light grid; jitter breaks the slice banding before the integrate.
// View space is +Z forward (LH LookAt + [2][3]=+1 projection), so visible rays have ViewRay.z > 0.
float3 FroxelToWorld(uint3 Coord, float3 Jitter)
{
    float2 UV  = (float2(Coord.xy) + Jitter.xy) / float2(Pass().GridSize.xy);
    float2 NDC = UV * 2.0 - 1.0;

    float4 ViewH   = mul(GetInverseCameraProjection(), float4(NDC, 1.0, 1.0));
    float3 ViewRay = ViewH.xyz / ViewH.w;            // point on the pixel ray in view space
    float  Dist    = Froxel_SliceDistance(float(Coord.z) + Jitter.z, float(Pass().GridSize.z), Pass().NearPlane, Pass().FogRange);
    float3 ViewPos = ViewRay * (Dist / max(ViewRay.z, 1e-4));   // view Z = +Dist
    return mul(GetInverseCameraView(), float4(ViewPos, 1.0)).xyz;
}

// Shadow is the only high-frequency term across a froxel, so only it is supersampled.
float SampleCloudShadow(float3 WorldPos)
{
    return CloudShadow_Sample(Pass().CloudShadowIndex, WorldPos, Pass().CloudShadowCenter, Pass().CloudShadowExtent);
}

float SampleSunShadowAA(FLight Sun, uint3 Coord)
{
    // 4 taps stratified across XY AND the slab's depth (Z) -- averaging the shadow over depth smooths
    // the exponential-Z slab stepping in shadow shafts deterministically (no grain), same 4-tap cost.
    const float3 Offs[4] = { float3(0.25, 0.25, 0.2), float3(0.75, 0.25, 0.4),
                             float3(0.25, 0.75, 0.6), float3(0.75, 0.75, 0.8) };
    float S = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float3 P = FroxelToWorld(Coord, Offs[i]);
        S += SampleSunCascadeByContainment(Sun, P) * SampleCloudShadow(P);
    }
    return S * 0.25;
}

// Spot lights rasterize one atlas tile; point lights rasterize six, picked by the light-to-froxel ray.
float SampleLocalShadow(FLight Light, float3 WorldPos)
{
    if (Light.ShadowDataIndex == INDEX_NONE)
    {
        return 1.0;
    }
    uint Face = HasFlag(Light.Flags, ELightFlags::Spot)
              ? 0u
              : uint(DirectionToCubemapCoord(WorldPos - Light.Position).Face);

    FLightShadow Tile = GetShadowTile(Light.ShadowDataIndex, Face);
    float4x4 VP = GetShadowViewProjection(Light.ShadowDataIndex, Face);
    float4   SC = mul(VP, float4(WorldPos, 1.0));
    float3   P  = SC.xyz / max(SC.w, 1e-6);
    if (P.z < 0.0 || P.z > 1.0)
    {
        return 1.0;
    }
    float2 UV  = saturate(P.xy * 0.5 + 0.5);
    float2 AUV = Tile.AtlasUVOffset + UV * Tile.AtlasUVScale;
    return ShadowAtlas().SampleCmpLevelZero(AUV, P.z - 0.0005);
}

// Lights come from the cluster grid, so the bound is LIGHTS_PER_CLUSTER locally, not a global cap.
float3 LocalLightsInScatter(FExponentialHeightFogParams Fog, uint ClusterIndex, float3 WorldPos, float3 ToCam)
{
    float  G         = Fog.VolumetricParams.y;
    float3 InScatter = float3(0.0);

    uint NumLocal = min(Clusters()[ClusterIndex].Count, (uint)LIGHTS_PER_CLUSTER);
    for (uint li = 0; li < NumLocal; ++li)
    {
        FLight L = GetLightAt(Clusters()[ClusterIndex].LightIndices[li]);
        if (!HasFlag(L.Flags, ELightFlags::Volumetric))
        {
            continue;
        }

        float3 ToL  = L.Position - WorldPos;
        float  Dist = length(ToL);
        if (Dist > L.Radius)
        {
            continue;
        }
        float3 Ldir  = ToL / max(Dist, 1e-4);
        // Soft-core 1/d^2: a finite source radius caps the apex peak so the near-light glow spreads
        // over several froxels instead of a single pixelated spike. Per-light (VolumetricScatteringRadius).
        float  R0    = L.Radius * L.VolumetricScatteringRadius;
        float  Atten = 1.0 / max(Dist * Dist + R0 * R0, 1e-6);
        float  DistR = Dist / L.Radius;
        Atten *= 1.0 - smoothstep(clamp(L.Falloff, 0.0, 1.0), 1.0, DistR);

        float Cone = 1.0;
        if (HasFlag(L.Flags, ELightFlags::Spot))
        {
            float3 SF    = normalize(L.Direction);   // to-light (surface->spot)
            float  CosAt = dot(SF, Ldir);
            Cone = saturate((CosAt - L.Angles.y) / max(L.Angles.x - L.Angles.y, 1e-3));
            if (Cone <= 0.0)
            {
                continue;
            }
        }

        float Shadow = SampleLocalShadow(L, WorldPos);

        // Ldir points toward the light; the photon travels the other way, so negate
        // to match the sun's travel-direction phase convention.
        float  PhaseShadow = Fog_MultiScatterPhaseShadow(Fog, dot(-Ldir, ToCam), G, Shadow);
        float3 LC          = UnpackColor(L.Color).rgb;
        InScatter += LC * L.Intensity * Atten * Cone * PhaseShadow * L.VolumetricIntensity;
    }
    return InScatter;
}

// The cluster grid slices view Z the same exponential way, just over the camera's full range.
uint FroxelClusterIndex(uint3 Coord)
{
    float2 UV        = (float2(Coord.xy) + 0.5) / float2(Pass().GridSize.xy);
    float2 ScreenPos = UV * float2(Scene().ScreenSize.xy);
    float  ViewZ     = Froxel_SliceDistance(float(Coord.z) + 0.5, float(Pass().GridSize.z), Pass().NearPlane, Pass().FogRange);
    return ComputeClusterIndex(ViewZ, ScreenPos);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void ComputeMain(uint3 DispatchID : SV_DispatchThreadID)
{
    if (any(DispatchID >= Pass().GridSize))
    {
        return;
    }

    // Froxels fully behind opaque geometry are never sampled by the apply pass (it reads at scene
    // depth, +-half a slice of dither plus one slice of linear filter), so skip their shadow taps.
    // Hi-Z min-reduction = farthest depth in the tile under reverse-Z; the level covers ~2x the
    // froxel footprint for conservatism, and the 2-slice margin covers the apply-side filtering.
    {
        float2 TileUV  = (float2(DispatchID.xy) + 0.5) / float2(Pass().GridSize.xy);
        float  TexelsX = Scene().CullData.PyramidWidth / float(Pass().GridSize.x);
        float  TexelsY = Scene().CullData.PyramidHeight / float(Pass().GridSize.y);
        float  Level   = max(ceil(log2(max(TexelsX, TexelsY))), 0.0);
        float  TileDepth = SampleTexture2DLevel(Scene().CullData.DepthPyramidIndex, SAMPLER_MIN_REDUCTION, TileUV, Level).x;
        if (TileDepth > 0.0)   // 0 = sky somewhere in the tile -> nothing to skip
        {
            float MaxTileDist = ReconstructViewPos(TileUV, TileDepth, GetInverseCameraProjection()).z;
            if (Froxel_SliceDistance(float(DispatchID.z) - 2.0, float(Pass().GridSize.z), Pass().NearPlane, Pass().FogRange) > MaxTileDist)
            {
                gRWTextures3D[Pass().ScatterUAV][DispatchID] = float4(0.0);
                return;
            }
        }
    }

    // Sample the slice center. Sun-shadow shafts are debanded by Z-stratified taps in
    // SampleSunShadowAA (averaging, not dither) so the smooth fog stays grain-free.
    FExponentialHeightFogParams uFog = Scene().FogParams;

    float3 WorldPos = FroxelToWorld(DispatchID, float3(0.5, 0.5, 0.5));
    float3 ToCamVec = GetCameraPosition() - WorldPos;
    float  CamDist  = length(ToCamVec);
    float3 ToCam    = ToCamVec / max(CamDist, 1e-4);

    // Sun-facing albedo tint applies whenever a directional light exists, independent of whether
    // it casts volumetric shafts; both this pass and the analytic far fog use the same albedo.
    bool bHasSun = GetNumLights() > 0 && HasFlag(GetLightAt(0).Flags, ELightFlags::Directional);
    // Light.Direction points from the surface toward the sun; the phase/tint want the photon
    // travel direction, so negate to get CosSun = +1 when looking straight at the sun.
    float CosSun = bHasSun ? dot(-normalize(GetLightAt(0).Direction), ToCam) : -1.0;

    // Global height profile times its noise, plus any local volumes, ramped past FogStartDistance.
    FFogMedium Medium = Fog_BeginMedium(uFog, WorldPos, Pass().Time, CamDist);

    uint NumVolumes = min(Pass().NumFogVolumes, FOG_MAX_VOLUMES);
    if (NumVolumes != 0)
    {
        FFogVolume* Volumes = (FFogVolume*)Pass().FogVolumes;
        for (uint vi = 0; vi < NumVolumes; ++vi)
        {
            Fog_AddVolume(Medium, Volumes[vi], WorldPos);
        }
    }
    Fog_FinalizeMedium(Medium);

    float  Ramp      = Fog_StartRamp(uFog, CamDist);
    float  SigmaT    = Medium.Extinction * Ramp;
    float  G         = uFog.VolumetricParams.y;
    float  Intensity = uFog.VolumetricParams.x * Medium.ScatterIntensity;

    // Volume albedo replaces the fog's own base albedo, then the sun-facing tint rides on top of both.
    float3 SigmaS = Fog_AlbedoFrom(uFog, Medium.ScatterAlbedo, CosSun, CamDist) * SigmaT;

    // Ambient (skylight) in-scatter, unshadowed, so shadowed fog reads as dim haze not a black void.
    // This is the skylight's OWN first bounce (already isotropic, already unshadowed), which is why it
    // is not run through the octave loop -- the octaves model further bounces of the DIRECTIONAL light
    // below, and folding the ambient in as well would double-count it. Raise AmbientIntensity for a
    // brighter floor; raise the octave count for shadow fill that follows the sun.
    float3 InScatter = GetVolumetricAmbientRadiance();

    // Sun.
    if (Pass().bSunVolumetric != 0)
    {
        FLight Sun      = GetLightAt(0);
        float  Shadow   = SampleSunShadowAA(Sun, DispatchID);
        float3 SunColor = UnpackColor(Sun.Color).rgb;
        InScatter += SunColor * Sun.Intensity * Fog_MultiScatterPhaseShadow(uFog, CosSun, G, Shadow) * Sun.VolumetricIntensity;
    }

    // Opted-in local point/spot lights. A single center sample aliases the bright near-light core
    // and cone edge into froxel-sized blocks, so (when enabled) supersample at 4 sub-froxel XY
    // positions and average -- the froxel stores the volume average, not a spike.
    uint ClusterIndex = FroxelClusterIndex(DispatchID);
    if (Pass().bSupersampleLocal != 0)
    {
        const float2 Offs[4] = { float2(0.25, 0.25), float2(0.75, 0.25),
                                 float2(0.25, 0.75), float2(0.75, 0.75) };
        float3 LocalSum = float3(0.0);
        [unroll]
        for (int s = 0; s < 4; ++s)
        {
            float3 SubPos   = FroxelToWorld(DispatchID, float3(Offs[s], 0.5));
            float3 SubToCam = normalize(GetCameraPosition() - SubPos);
            LocalSum += LocalLightsInScatter(uFog, ClusterIndex, SubPos, SubToCam);
        }
        InScatter += LocalSum * 0.25;
    }
    else
    {
        InScatter += LocalLightsInScatter(uFog, ClusterIndex, WorldPos, ToCam);
    }

    // Emissive volumes glow on their own, so it rides outside the extinction-weighted scattering term.
    float3 Scatter = SigmaS * InScatter * Intensity + Medium.Emissive * Ramp;
    gRWTextures3D[Pass().ScatterUAV][DispatchID] = float4(Scatter, SigmaT);
}
