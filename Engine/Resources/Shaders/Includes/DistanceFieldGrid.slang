#pragma once
#include "Includes/DistanceField.slang"

// Mesh distance fields culled for one frame and binned into a 2D grid on a plane, shared by the sun's far shadows and the sky's occlusion.

static const uint kDFGridObjectCounter = 0u;
static const uint kDFGridLargeCounter  = 1u;
static const uint kDFMaxStepsPerObject = 32u;

// Past this many cells an object is binned by a whole group, since one lane walking them in turn stalls the pass.
static const uint kDFGridLargeObjectCells = 64u;

// Set on every object over kDFGridLargeObjectCells so the per-lane binning skips it, which drops one the full large list could not take.
static const uint kDFGridLargeObject = 2u;

// Mirror of Lumina::FDFObject (112 B). One object, already inverted so a trace never touches its transform.
struct FDFObject
{
    FTransform3x4 WorldToLocal;
    float4        Sphere;
    float3        VolumeMin;
    float         MaxDistance;
    float3        VolumeSize;
    float         MinScale;
    FTexture3D    Texture;
    uint          Flags;
    float         VoxelSize;
    uint          _Pad0;
};

// Mirror of Lumina::FDFObjectGrid. A square grid spanned by Right and Up, each object binned over its sphere grown by SpherePad.
struct FDFObjectGrid
{
    GPUSpan<FDFObject> Objects;
    // One past the cells, and after binning each holds the cell's first item while the extra slot holds the total.
    GPUSpan<uint>      CellCounts;
    GPUSpan<uint>      CellItems;
    GPUSpan<uint>      Counters;
    float3             Right;
    float              CellSize;
    float3             Up;
    float              MinRadius;
    float2             Origin;
    uint               Dimension;
    float              SpherePad;

    // The cell under a world point, negative or past Dimension when the point is off the grid.
    int2 CellAt(float3 P)
    {
        return int2(floor((float2(dot(P, Right), dot(P, Up)) - Origin) / CellSize));
    }

    // Unclamped first cell in xy and last in zw that a sphere grown by SpherePad covers.
    float4 CellBounds(float4 Sphere)
    {
        const float2 XY     = float2(dot(Sphere.xyz, Right), dot(Sphere.xyz, Up));
        const float  Radius = Sphere.w + SpherePad;
        return float4(floor((XY - Radius - Origin) / CellSize), floor((XY + Radius - Origin) / CellSize));
    }

    // CellBounds clamped after the conversion, so no float it mishandles can widen a loop past the grid.
    int4 CellRange(float4 Sphere)
    {
        const float4 Bounds   = CellBounds(Sphere);
        const int    LastCell = int(Dimension) - 1;
        const int2   First = clamp(int2(max(Bounds.xy, 0.0)), int2(0, 0), int2(LastCell, LastCell));
        const int2   Last  = clamp(int2(min(Bounds.zw, float2(LastCell, LastCell))), int2(0, 0), int2(LastCell, LastCell));
        return int4(First, Last);
    }
};

float SampleDFObject(FDFObject Object, float3 LocalPosition)
{
    const float3 UVW = (LocalPosition - Object.VolumeMin) / max(Object.VolumeSize, float3(1e-6));
    const float  Encoded = Object.Texture
        .SampleLevel(Samplers::LinearClamp, UVW, 0.0).r;
    return (Object.Flags & kDistanceFieldTwoSided) != 0u
        ? Encoded * Object.MaxDistance
        : (Encoded * 2.0 - 1.0) * Object.MaxDistance;
}

float3 DFObjectLocalPoint(FDFObject Object, float3 P)
{
    return float3(dot(Object.WorldToLocal.Row0, float4(P, 1.0)), dot(Object.WorldToLocal.Row1, float4(P, 1.0)), dot(Object.WorldToLocal.Row2, float4(P, 1.0)));
}

float3 DFObjectLocalVector(FDFObject Object, float3 V)
{
    return float3(dot(Object.WorldToLocal.Row0.xyz, V), dot(Object.WorldToLocal.Row1.xyz, V), dot(Object.WorldToLocal.Row2.xyz, V));
}

// Sphere traces a cone through one object in its own space, parametrized by world distance so the cone stays in world units.
float TraceDFObjectCone(FDFObject Object, float3 P, float3 Dir, float TanHalfAngle, float MaxTrace)
{
    const float3 LocalOrigin = DFObjectLocalPoint(Object, P);
    const float3 LocalDir    = DFObjectLocalVector(Object, Dir);

    const float3 SafeDir = float3(abs(LocalDir.x) < 1e-8 ? 1e-8 : LocalDir.x,
                                  abs(LocalDir.y) < 1e-8 ? 1e-8 : LocalDir.y,
                                  abs(LocalDir.z) < 1e-8 ? 1e-8 : LocalDir.z);
    const float3 T0 = (Object.VolumeMin - LocalOrigin) / SafeDir;
    const float3 T1 = (Object.VolumeMin + Object.VolumeSize - LocalOrigin) / SafeDir;
    const float3 Near = min(T0, T1);
    const float3 Far  = max(T0, T1);

    // A voxel's worth of world travel along this ray, which keeps the receiver from occluding itself.
    const float LocalPerWorld = max(length(LocalDir), 1e-8);
    const float VoxelLocal    = Object.VoxelSize;
    const float VoxelWorld    = VoxelLocal / LocalPerWorld;

    float       t    = max(max(Near.x, max(Near.y, Near.z)), 2.0 * VoxelWorld);
    const float TEnd = min(min(Far.x, min(Far.y, Far.z)), MaxTrace);

    const bool  bTwoSided  = (Object.Flags & kDistanceFieldTwoSided) != 0u;
    float       Visibility = 1.0;
    [loop]
    for (uint Step = 0; Step < kDFMaxStepsPerObject && t < TEnd; ++Step)
    {
        float Local = SampleDFObject(Object, LocalOrigin + LocalDir * t);
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
