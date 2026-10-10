#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/ShadowSampling.slang"
#include "Includes/Fog.slang"
#include "Includes/Froxel.slang"
#include "Includes/CloudCommon.slang"

struct FPushConstants
{
    uint3  GridSize;
    float  NearPlane;

    float        FogRange;
    uint         bSunVolumetric;
    float        Time;
    FRWTexture3D Scatter;

    uint       LocalLightSamples;   // 0 skips local lights, 1 is the froxel center, 4 supersamples
    uint       _PadNumFogVolumes;
    FTexture2D CloudShadow;    // invalid when no cloud shadow was built
    float      CloudShadowExtent;

    float2 CloudShadowCenter;
    float2 _Pad0;

    // Pointer last so its 8-byte alignment lands on a multiple of 8.
    GPUSpan<FFogVolume> FogVolumes;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// View space is +Z forward, so visible rays have ViewRay.z > 0; slices match the clustered light grid.
float3 FroxelToWorld(uint3 Coord, float3 Jitter)
{
    float2 UV  = (float2(Coord.xy) + Jitter.xy) / float2(Pass().GridSize.xy);
    float2 NDC = UV * 2.0 - 1.0;

    float4 ViewH   = mul(GetInverseCameraProjection(), float4(NDC, 1.0, 1.0));
    float3 ViewRay = ViewH.xyz / ViewH.w;
    float  Dist    = Froxel_SliceDistance(float(Coord.z) + Jitter.z, float(Pass().GridSize.z), Pass().NearPlane, Pass().FogRange);
    float3 ViewPos = ViewRay * (Dist / max(ViewRay.z, 1e-4));
    return mul(GetInverseCameraView(), float4(ViewPos, 1.0)).xyz;
}

// Shadow is the only high-frequency term across a froxel, so only it is supersampled.
float SampleCloudShadow(float3 WorldPos)
{
    return CloudShadow_Sample(Pass().CloudShadow, WorldPos, Pass().CloudShadowCenter, Pass().CloudShadowExtent);
}

float SampleSunShadowAA(FLight Sun, uint3 Coord)
{
    // Taps stratified across the slab's depth too, which smooths exponential-Z stepping in shafts without grain.
    const float3 Offs[4] = { float3(0.25, 0.25, 0.2), float3(0.75, 0.25, 0.4),
                             float3(0.25, 0.75, 0.6), float3(0.75, 0.75, 0.8) };
    float S = 0.0;
    // Each copy holds a whole cascade search plus a cloud shadow tap live.
    [loop]
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
    if (Tile.ShadowMapIndex < 0)
    {
        return 1.0;
    }
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

static const uint kMaxLocalSamples = 4;

// Every sample shares one walk of the cluster list, so a supersampled froxel fetches each light once.
float3 LocalLightsInScatter(FExponentialHeightFogParams Fog, uint ClusterIndex, float3 SamplePos[kMaxLocalSamples],
                            float3 SampleToCam[kMaxLocalSamples], uint NumSamples)
{
    float  G         = Fog.VolumetricParams.y;
    float3 InScatter = float3(0.0);

    const uint  MaskWords = GetClusterMaskWords();
    const uint2 Words     = GetClusterWordRange(ClusterIndex);
    uint NumLights  = GetNumLights();
    for (uint Word = Words.x; Word < Words.y; ++Word)
    {
        uint Pending = GetClusterMaskWord(ClusterIndex, Word, MaskWords);
        while (Pending != 0u)
        {
            uint LightIndex = Word * 32u + firstbitlow(Pending);
            Pending &= Pending - 1u;
            // Cluster contents go stale across a scene switch and the array holds exactly NumLights entries.
            if (LightIndex >= NumLights)
            {
                continue;
            }

            FLight L = GetLightAt(LightIndex);
            if (!HasFlag(L.Flags, ELightFlags::Volumetric))
            {
                continue;
            }

            // A soft core, so the source radius spreads the near-light glow over several froxels instead of one spike.
            // An area light keeps its right axis where the core radius would be, so its core is the rectangle's own half size.
            const bool   bArea = HasFlag(L.Flags, ELightFlags::Area);
            const float  R0    = bArea ? max(L.Angles.x, L.Angles.y) : L.Radius * L.VolumetricScatteringRadius;
            const float3 LC    = UnpackColor(L.Color).rgb * (L.Intensity * L.VolumetricIntensity);
            const bool   bSpot = HasFlag(L.Flags, ELightFlags::Spot);
            const float3 SF    = (bSpot || bArea) ? normalize(L.Direction) : float3(0.0);

            for (uint s = 0; s < NumSamples; ++s)
            {
                const float3 WorldPos = SamplePos[s];
                float3 ToL  = L.Position - WorldPos;
                float  Dist = length(ToL);
                if (Dist > L.Radius)
                {
                    continue;
                }
                float3 Ldir  = ToL / max(Dist, 1e-4);
                float  Atten = 1.0 / max(Dist * Dist + R0 * R0, 1e-6);
                float  DistR = Dist / L.Radius;
                Atten *= 1.0 - smoothstep(clamp(L.Falloff, 0.0, 1.0), 1.0, DistR);

                float Cone = 1.0;
                if (bArea)
                {
                    Cone = saturate(dot(SF, Ldir));
                    if (Cone <= 0.0)
                    {
                        continue;
                    }
                }
                else if (bSpot)
                {
                    float CosAt = dot(SF, Ldir);
                    Cone = saturate((CosAt - L.Angles.y) / max(L.Angles.x - L.Angles.y, 1e-3));
                    if (Cone <= 0.0)
                    {
                        continue;
                    }
                }

                float Shadow = SampleLocalShadow(L, WorldPos);

                // Negated because Ldir points at the light while the photon travels away from it, as the sun's phase expects.
                float PhaseShadow = Fog_MultiScatterPhaseShadow(Fog, dot(-Ldir, SampleToCam[s]), G, Shadow);
                InScatter += LC * Atten * Cone * PhaseShadow * LightFunctionMask(L, Ldir, WorldPos);
            }
        }
    }
    return InScatter / float(max(NumSamples, 1u));
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

    // Froxels behind opaque geometry (with a 2-slice margin for the apply pass's dither and filter) are never sampled.
    {
        float2 TileUV  = (float2(DispatchID.xy) + 0.5) / float2(Pass().GridSize.xy);
        float  TexelsX = Scene().CullData.PyramidWidth / float(Pass().GridSize.x);
        float  TexelsY = Scene().CullData.PyramidHeight / float(Pass().GridSize.y);
        float  Level   = max(ceil(log2(max(TexelsX, TexelsY))), 0.0);
        float  TileDepth = Scene().CullData.DepthPyramid.SampleLevel(Samplers::MinReduction, TileUV, Level).x;
        if (TileDepth > 0.0)   // 0 means sky somewhere in the tile, so nothing to skip
        {
            float MaxTileDist = ReconstructViewPos(TileUV, TileDepth, GetInverseCameraProjection()).z;
            if (Froxel_SliceDistance(float(DispatchID.z) - 2.0, float(Pass().GridSize.z), Pass().NearPlane, Pass().FogRange) > MaxTileDist)
            {
                Pass().Scatter[DispatchID] = float4(0.0);
                return;
            }
        }
    }

    FExponentialHeightFogParams uFog = Scene().FogParams;

    float3 WorldPos = FroxelToWorld(DispatchID, float3(0.5, 0.5, 0.5));
    float3 ToCamVec = GetCameraPosition() - WorldPos;
    float  CamDist  = length(ToCamVec);
    float3 ToCam    = ToCamVec / max(CamDist, 1e-4);

    // The tint applies whenever a directional light exists, matching the analytic far fog's albedo.
    bool bHasSun = GetNumLights() > 0 && HasFlag(GetLightAt(0).Flags, ELightFlags::Directional);
    // Negated so CosSun is +1 when looking straight at the sun.
    float CosSun = bHasSun ? dot(-normalize(GetLightAt(0).Direction), ToCam) : -1.0;

    // Global height profile times its noise, plus any local volumes, ramped past FogStartDistance.
    FFogMedium Medium = Fog_BeginMedium(uFog, WorldPos, Pass().Time, CamDist);

    uint NumVolumes = min(Pass().FogVolumes.Count, (uint)FOG_MAX_VOLUMES);
    for (uint vi = 0; vi < NumVolumes; ++vi)
    {
        Fog_AddVolume(Medium, Pass().FogVolumes.Get(vi), WorldPos);
    }
    Fog_FinalizeMedium(Medium);

    float  Ramp      = Fog_StartRamp(uFog, CamDist);
    float  SigmaT    = Medium.Extinction * Ramp;
    float  G         = uFog.VolumetricParams.y;
    float  Intensity = uFog.VolumetricParams.x * Medium.ScatterIntensity;

    // Volume albedo replaces the fog's own base albedo, then the sun-facing tint rides on top of both.
    float3 SigmaS = Fog_AlbedoFrom(uFog, Medium.ScatterAlbedo, CosSun, CamDist) * SigmaT;

    // The skylight's own first bounce, kept out of the octave loop since the octaves model the directional light and would double-count it.
    float3 InScatter = GetVolumetricAmbientRadiance();

    if (Pass().bSunVolumetric != 0)
    {
        FLight Sun      = GetLightAt(0);
        float  Shadow   = SampleSunShadowAA(Sun, DispatchID);
        float3 SunColor = UnpackColor(Sun.Color).rgb;
        InScatter += SunColor * Sun.Intensity * Fog_MultiScatterPhaseShadow(uFog, CosSun, G, Shadow) * Sun.VolumetricIntensity
                   * LightFunctionMask(Sun, Sun.Direction, WorldPos);
    }

    // Supersampled because one center sample aliases the near-light core and cone edge into froxel-sized blocks.
    const uint NumSamples = min(Pass().LocalLightSamples, kMaxLocalSamples);
    if (NumSamples > 0)
    {
        const float2 Offs[kMaxLocalSamples] = { float2(0.25, 0.25), float2(0.75, 0.25),
                                                float2(0.25, 0.75), float2(0.75, 0.75) };
        float3 SamplePos[kMaxLocalSamples];
        float3 SampleToCam[kMaxLocalSamples];
        [unroll]
        for (uint s = 0; s < kMaxLocalSamples; ++s)
        {
            SamplePos[s]   = NumSamples == 1 ? WorldPos : FroxelToWorld(DispatchID, float3(Offs[s], 0.5));
            SampleToCam[s] = NumSamples == 1 ? ToCam : normalize(GetCameraPosition() - SamplePos[s]);
        }
        InScatter += LocalLightsInScatter(uFog, FroxelClusterIndex(DispatchID), SamplePos, SampleToCam, NumSamples);
    }

    // Emissive volumes glow on their own, so it rides outside the extinction-weighted scattering term.
    float3 Scatter = SigmaS * InScatter * Intensity + Medium.Emissive * Ramp;
    Pass().Scatter[DispatchID] = float4(Scatter, SigmaT);
}
