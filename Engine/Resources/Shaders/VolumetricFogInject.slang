#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/ShadowSampling.slang"
#include "Includes/Fog.slang"
#include "Includes/Froxel.slang"

// Froxel volumetric fog inject pass: per froxel, write exp-height-fog extinction plus shadowed
// in-scatter from the sun + opted-in local lights into uFroxelScatter; the integrate pass accumulates.

#define FROXEL_MAX_LOCAL_LIGHTS 16

struct FPushConstants
{
    uint3  GridSize;
    float  NearPlane;
    float  FogRange;
    uint   bSunVolumetric;
    uint   NumLocalVolumetric;
    float  Time;
    uint   LocalLightIndices[FROXEL_MAX_LOCAL_LIGHTS];
    // Pointer placed after the array so its 8-byte alignment lands on a multiple of 8 (offset 96).
    uint64_t Fog;         // FExponentialHeightFogParams (transient device address), see LoadBlock
    uint   ScatterUAV;   // bindless 3D UAV index of the froxel scatter volume
    uint   bSupersampleLocal;   // 1 = 4x supersample local light in-scatter per froxel
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

// Pick the first cascade whose ortho frustum contains the froxel. Containment is independent of the
// view-Z sign convention, unlike the lit pass's split-distance test (which assumes positive view distance).
float SampleSunShadow(FLight Light, float3 WorldPos)
{
    if (Light.ShadowDataIndex == INDEX_NONE)
    {
        return 1.0;
    }

    [unroll]
    for (int c = 0; c < NUM_CASCADES; ++c)
    {
        float4x4 VP = GetShadowViewProjection(Light.ShadowDataIndex, c);
        float4   SC = mul(VP, float4(WorldPos, 1.0));
        float3   P  = SC.xyz / max(SC.w, 1e-6);
        float2   UV = P.xy * 0.5 + 0.5;

        if (all(UV >= 0.0) && all(UV <= 1.0) && P.z >= 0.0 && P.z <= 1.0)
        {
            FLightShadow Tile = GetShadowTile(Light.ShadowDataIndex, c);
            float2 AUV = UV * Tile.AtlasUVScale + Tile.AtlasUVOffset;
            return ShadowCascade().SampleCmpLevelZero(AUV, P.z - 0.0005);
        }
    }
    return 1.0;
}

// Shadow is the only high-frequency term across a froxel, so supersample just it at four sub-froxel
// positions and average. Soft god-ray edges without temporal accumulation.
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
        S += SampleSunShadow(Sun, FroxelToWorld(Coord, Offs[i]));
    }
    return S * 0.25;
}

float SampleSpotShadow(FLight Light, float3 WorldPos)
{
    if (Light.ShadowDataIndex == INDEX_NONE)
    {
        return 1.0;
    }
    FLightShadow Tile = GetShadowTile(Light.ShadowDataIndex, 0);
    float4x4 VP = GetShadowViewProjection(Light.ShadowDataIndex, 0);
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

// In-scatter from the opted-in local point/spot lights at one world position. Pulled out of
// ComputeMain so it can be supersampled at several sub-froxel positions (the near-light core and
// cone edge are far higher frequency than the froxel grid).
float3 LocalLightsInScatter(float3 WorldPos, float3 ToCam, float G)
{
    float3 InScatter = float3(0.0);
    uint NumLocal = min(Pass().NumLocalVolumetric, (uint)FROXEL_MAX_LOCAL_LIGHTS);
    for (uint li = 0; li < NumLocal; ++li)
    {
        FLight L    = GetLightAt(Pass().LocalLightIndices[li]);
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

        float Shadow = 1.0;
        if (L.ShadowDataIndex != INDEX_NONE && HasFlag(L.Flags, ELightFlags::Spot))
        {
            Shadow = SampleSpotShadow(L, WorldPos);
        }

        // Ldir points toward the light; the photon travels the other way, so negate
        // to match the sun's travel-direction phase convention.
        float  Phase = Fog_Phase(dot(-Ldir, ToCam), G);
        float3 LC    = UnpackColor(L.Color).rgb;
        InScatter += LC * L.Intensity * Atten * Cone * Phase * Shadow * L.VolumetricIntensity;
    }
    return InScatter;
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
    FExponentialHeightFogParams uFog = LoadBlock<FExponentialHeightFogParams>(Pass().Fog);

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

    // Extinction from the exp-height profile (clamped so it can't explode below base),
    // ramped in past FogStartDistance.
    float  Density   = clamp(Fog_DensityAt(uFog, WorldPos), 0.0, uFog.InscatteringColor.w);
    Density         *= Fog_StartRamp(uFog, CamDist);
    float  SigmaT    = Density;
    float3 SigmaS    = Fog_Albedo(uFog, CosSun, CamDist) * Density;   // scattering = albedo * density
    float  G         = uFog.VolumetricParams.y;
    float  Intensity = uFog.VolumetricParams.x;

    // Ambient (skylight) in-scatter, unshadowed, so shadowed fog reads as dim haze not a black void.
    // Cheap multi-scatter stand-in; raise AmbientIntensity for brighter shadows.
    float3 InScatter = GetAmbientLightColor() * GetAmbientLightIntensity();

    // Sun.
    if (Pass().bSunVolumetric != 0)
    {
        FLight Sun     = GetLightAt(0);
        float  Phase   = Fog_Phase(CosSun, G);
        float  Shadow  = SampleSunShadowAA(Sun, DispatchID);
        float3 SunColor = UnpackColor(Sun.Color).rgb;
        InScatter += SunColor * Sun.Intensity * Phase * Shadow * Sun.VolumetricIntensity;
    }

    // Opted-in local point/spot lights. A single center sample aliases the bright near-light core
    // and cone edge into froxel-sized blocks, so (when enabled) supersample at 4 sub-froxel XY
    // positions and average -- the froxel stores the volume average, not a spike.
    if (Pass().NumLocalVolumetric != 0)
    {
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
                LocalSum += LocalLightsInScatter(SubPos, SubToCam, G);
            }
            InScatter += LocalSum * 0.25;
        }
        else
        {
            InScatter += LocalLightsInScatter(WorldPos, ToCam, G);
        }
    }

    float3 Scatter = SigmaS * InScatter * Intensity;
    gRWTextures3D[Pass().ScatterUAV][DispatchID] = float4(Scatter, SigmaT);
}
