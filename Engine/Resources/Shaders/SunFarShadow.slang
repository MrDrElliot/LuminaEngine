#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/WorldDistanceField.slang"
#include "Includes/TerrainHeightfield.slang"

// Sun occlusion the virtual shadow levels cannot hold, marched per pixel from the depth buffer and read back by the lighting.

// Mirror of Lumina::FSunFarShadowArgs.
struct FPushConstants
{
    uint2                ScreenSize;
    FTexture2D           Depth;
    FRWTexture2D         Output;
    float3               ToSun;
    float                TanHalfAngle;
    float                TerrainMaxDistance;
    uint                 TerrainSteps;
    uint                 NumTerrains;
    float                _Pad;
    FWorldDistanceField  Field;
    float                DFNear;
    float                DFFar;
    float                DFMaxTrace;
    uint                 bDistanceField;
    FTerrainHeightfield Terrains[kMaxTerrainHeightfields];
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Steps toward the sun through the world field, at least half a voxel at a time so a grazing ray still leaves.
static const uint kSunFieldSteps = 64u;

// Starts a voxel and a half toward the sun, past the receiver's own surface in the field, since a depth normal on distant foliage is only noise.
float TraceSunField(FWorldDistanceField* Field, float3 P, float3 ToSun, float TanHalfAngle, float MaxTrace)
{
    const uint ReceiverLevel = Field.LevelAt(P);
    if (ReceiverLevel >= kWorldDFLevels)
    {
        return 1.0;
    }

    float Visibility = 1.0;
    float t = 1.5 * Field.VoxelSize(ReceiverLevel);
    [loop]
    for (uint Step = 0u; Step < kSunFieldSteps && t < MaxTrace; ++Step)
    {
        uint Level;
        const float Distance = Field.Sample(P + ToSun * t, Level);
        if (Level >= kWorldDFLevels)
        {
            break;
        }
        const float Voxel = Field.VoxelSize(Level);
        Visibility = min(Visibility, saturate(Distance / max(t * TanHalfAngle, 0.5 * Voxel)));
        if (Visibility <= 0.0)
        {
            break;
        }
        t += max(Distance, 0.5 * Voxel);
    }
    return Visibility;
}

// Entry and exit distances of a ray through the terrain's square footprint, empty when it misses.
float2 TerrainFootprintRange(FTerrainHeightfield T, float3 Origin, float3 Dir)
{
    const float  Extent = T.Stride * (T.Resolution - 1.0);
    const float2 D  = float2(abs(Dir.x) < 1e-6 ? 1e-6 : Dir.x, abs(Dir.z) < 1e-6 ? 1e-6 : Dir.z);
    const float2 T0 = (T.OriginXZ - Origin.xz) / D;
    const float2 T1 = (T.OriginXZ + Extent - Origin.xz) / D;
    const float2 Near = min(T0, T1);
    const float2 Far  = max(T0, T1);
    return float2(max(Near.x, Near.y), min(Far.x, Far.y));
}

// Unreal's distance-field penumbra applied to a height field, the closest the ray passes over the ground relative to its cone.
float TraceTerrainShadow(FTerrainHeightfield T, float3 P, float ViewDistance)
{
    const float3 Dir = Pass().ToSun;
    const float  TopY = T.OriginY + T.MaxHeight;
    if (P.y >= TopY)
    {
        return 1.0;
    }

    const float2 Footprint = TerrainFootprintRange(T, P, Dir);
    const float  TopExit   = (TopY - P.y) / max(Dir.y, 1e-4);

    // Bilinear heights sit a little off the triangulated surface, so the march starts past a texel and a hair above it.
    const float StartBias = max(T.Stride, ViewDistance * 0.002);
    const float TStart    = max(Footprint.x, StartBias);
    const float TEnd      = min(min(Footprint.y, TopExit), Pass().TerrainMaxDistance);
    if (TStart >= TEnd)
    {
        return 1.0;
    }

    const float HeightBias = T.Stride * 0.25;
    const float Ratio      = TEnd / TStart;
    const uint  Steps      = max(Pass().TerrainSteps, 1u);

    float Visibility = 1.0;
    [loop]
    for (uint i = 0; i < Steps; ++i)
    {
        const float  t  = TStart * pow(Ratio, (float(i) + 0.5) / float(Steps));
        const float3 Q  = P + Dir * t;
        const float  Clearance = Q.y + HeightBias - T.HeightAt(Q.xz);
        Visibility = min(Visibility, saturate(Clearance / (t * Pass().TanHalfAngle + HeightBias)));
        if (Visibility <= 0.0)
        {
            break;
        }
    }
    return Visibility;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 Pixel = DTid.xy;
    if (any(Pixel >= Pass().ScreenSize))
    {
        return;
    }

    // Reversed Z clears to zero, so the sky stays lit.
    const float Raw = Pass().Depth.Load(int3(Pixel, 0)).r;
    float Visibility = 1.0;
    if (Raw > 0.0 && Pass().ToSun.y > 0.0)
    {
        const float2 UV = (float2(Pixel) + 0.5) / float2(Pass().ScreenSize);
        const float3 P  = LiftOntoTerrains(ReconstructWorldPos(UV, Raw), Pass().Terrains, Pass().NumTerrains);
        const float  ViewDistance = length(P - GetCameraPosition());

        [loop]
        for (uint i = 0; i < min(Pass().NumTerrains, kMaxTerrainHeightfields) && Visibility > 0.0; ++i)
        {
            Visibility = min(Visibility, TraceTerrainShadow(Pass().Terrains[i], P, ViewDistance));
        }

        // Meshes take over from the virtual shadow levels here and dissolve over the last tenth of their range.
        if (Pass().bDistanceField != 0u && Visibility > 0.0 && ViewDistance >= Pass().DFNear && ViewDistance <= Pass().DFFar)
        {
            const float  Traced  = TraceSunField(&Pass().Field, P, Pass().ToSun, Pass().TanHalfAngle, Pass().DFMaxTrace);
            const float Fade   = saturate((ViewDistance - Pass().DFFar * 0.9) / (Pass().DFFar * 0.1));
            Visibility = min(Visibility, lerp(Traced, 1.0, Fade));
        }
    }

    Pass().Output[Pixel] = float4(Visibility, 0.0, 0.0, 0.0);
}
