#pragma once
#include "Includes/DistanceField.slang"

// Shared by the caster binning pass and the per-pixel trace of the sun's distance-field shadows.

static const uint kSunDFObjectCounter   = 0u;
static const uint kSunDFMaxStepsPerObject = 32u;

// Mirror of Lumina::FSunDFObject (112 B). One caster, already inverted so the trace never touches its transform.
struct FSunDFObject
{
    FTransform3x4 WorldToLocal;
    float4        Sphere;
    float3        VolumeMin;
    float         MaxDistance;
    float3        VolumeSize;
    float         MinScale;
    FTexture3D    Texture;
    uint          Flags;
    uint          _Pad0;
    uint          _Pad1;
};

// Mirror of Lumina::FSunDFShadowGrid. A square grid in the plane facing the sun, covering every receiver this frame.
struct FSunDFShadowGrid
{
    GPUSpan<FSunDFObject> Objects;
    GPUSpan<uint>         CellCounts;
    GPUSpan<uint>         CellItems;
    GPUSpan<uint>         Counters;
    float3                SunRight;
    float                 CellSize;
    float3                SunUp;
    float                 MinRadius;
    float2                Origin;
    uint                  Dimension;
    uint                  CellCapacity;
};

float SampleSunDFObject(FSunDFObject Object, float3 LocalPosition)
{
    const float3 UVW = (LocalPosition - Object.VolumeMin) / max(Object.VolumeSize, float3(1e-6));
    const float  Encoded = Object.Texture
        .SampleLevel(Samplers::LinearClamp, UVW, 0.0).r;
    return (Object.Flags & kDistanceFieldTwoSided) != 0u
        ? Encoded * Object.MaxDistance
        : (Encoded * 2.0 - 1.0) * Object.MaxDistance;
}

// Sphere traces one caster in its own space, parametrized by world distance so the penumbra cone stays in world units.
float TraceSunDFObject(FSunDFObject Object, float3 P, float3 ToSun, float TanHalfAngle, float MaxTrace)
{
    const float3 LocalOrigin = float3(dot(Object.WorldToLocal.Row0, float4(P, 1.0)),
                                      dot(Object.WorldToLocal.Row1, float4(P, 1.0)),
                                      dot(Object.WorldToLocal.Row2, float4(P, 1.0)));
    const float3 LocalDir    = float3(dot(Object.WorldToLocal.Row0.xyz, ToSun),
                                      dot(Object.WorldToLocal.Row1.xyz, ToSun),
                                      dot(Object.WorldToLocal.Row2.xyz, ToSun));

    const float3 SafeDir = float3(abs(LocalDir.x) < 1e-8 ? 1e-8 : LocalDir.x,
                                  abs(LocalDir.y) < 1e-8 ? 1e-8 : LocalDir.y,
                                  abs(LocalDir.z) < 1e-8 ? 1e-8 : LocalDir.z);
    const float3 T0 = (Object.VolumeMin - LocalOrigin) / SafeDir;
    const float3 T1 = (Object.VolumeMin + Object.VolumeSize - LocalOrigin) / SafeDir;
    const float3 Near = min(T0, T1);
    const float3 Far  = max(T0, T1);

    // A voxel's worth of world travel along this ray, which keeps the receiver from shadowing itself.
    const float LocalPerWorld = max(length(LocalDir), 1e-8);
    const float VoxelLocal    = max(Object.VolumeSize.x, max(Object.VolumeSize.y, Object.VolumeSize.z)) / 32.0;
    const float VoxelWorld    = VoxelLocal / LocalPerWorld;

    float       t    = max(max(Near.x, max(Near.y, Near.z)), 2.0 * VoxelWorld);
    const float TEnd = min(min(Far.x, min(Far.y, Far.z)), MaxTrace);

    const bool  bTwoSided  = (Object.Flags & kDistanceFieldTwoSided) != 0u;
    float       Visibility = 1.0;
    [loop]
    for (uint Step = 0; Step < kSunDFMaxStepsPerObject && t < TEnd; ++Step)
    {
        float Local = SampleSunDFObject(Object, LocalOrigin + LocalDir * t);
        if (bTwoSided)
        {
            Local -= VoxelLocal * 0.5;
        }
        const float World = Local * Object.MinScale;
        Visibility = min(Visibility, saturate(World / max(t * TanHalfAngle, 0.25 * VoxelWorld)));
        if (Visibility <= 0.0)
        {
            break;
        }
        t += max(Local / LocalPerWorld, 0.5 * VoxelWorld);
    }
    return Visibility;
}

// Every caster binned into the receiver's cell, which is the only cell a ray toward the sun can cross.
float TraceSunDFShadows(FSunDFShadowGrid* Grid, float3 P, float3 ToSun, float TanHalfAngle, float MaxTrace)
{
    const float2 SunXY = float2(dot(P, Grid.SunRight), dot(P, Grid.SunUp));
    const int2   Cell  = int2(floor((SunXY - Grid.Origin) / Grid.CellSize));
    if (any(Cell < 0) || any(Cell >= int(Grid.Dimension)))
    {
        return 1.0;
    }

    const uint CellIndex = uint(Cell.y) * Grid.Dimension + uint(Cell.x);
    const uint Count     = min(Grid.CellCounts.Get(CellIndex), Grid.CellCapacity);

    float Visibility = 1.0;
    [loop]
    for (uint i = 0; i < Count && Visibility > 0.0; ++i)
    {
        const uint         ObjectIndex = Grid.CellItems.Get(CellIndex * Grid.CellCapacity + i);
        const FSunDFObject Object      = Grid.Objects.Get(ObjectIndex);

        // Rejects casters the ray passes beside or that sit behind the receiver.
        const float3 ToCenter = Object.Sphere.xyz - P;
        const float  Along    = dot(ToCenter, ToSun);
        const float  MissSq   = dot(ToCenter, ToCenter) - Along * Along;
        if (Along < -Object.Sphere.w || Along - Object.Sphere.w > MaxTrace || MissSq > Object.Sphere.w * Object.Sphere.w)
        {
            continue;
        }
        Visibility = min(Visibility, TraceSunDFObject(Object, P, ToSun, TanHalfAngle, MaxTrace));
    }
    return Visibility;
}
