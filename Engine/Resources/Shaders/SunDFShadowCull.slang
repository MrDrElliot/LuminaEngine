#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/DistanceField.slang"
#include "Includes/SunDFShadowCommon.slang"

// Gathers every static caster with a distance field and bins it into the sun-space grid the trace walks.

static const uint LOCAL_SIZE_X = 64u;

// Mirror of Lumina::FSunDFShadowCullArgs.
struct FPushConstants
{
    // Blocks of 64 slots holding a distance-field caster. Empty walks every slot.
    GPUSpan<uint>               Blocks;
    GPUSpan<FInstanceCullEntry> RetainedCullEntries;
    GPUSpan<FTransform3x4>      RetainedTransforms;
    GPUSpan<FInstanceStatic>    RetainedStatic;
    FSunDFShadowGrid            Grid;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Inverse of a rotation-scale-translation 3x4, through the adjugate so skew from a parent's scale stays exact.
FTransform3x4 InverseAffine(FTransform3x4 M)
{
    const float3 C0 = float3(M.Row0.x, M.Row1.x, M.Row2.x);
    const float3 C1 = float3(M.Row0.y, M.Row1.y, M.Row2.y);
    const float3 C2 = float3(M.Row0.z, M.Row1.z, M.Row2.z);
    const float3 T  = float3(M.Row0.w, M.Row1.w, M.Row2.w);

    const float3 R0 = cross(C1, C2);
    const float3 R1 = cross(C2, C0);
    const float3 R2 = cross(C0, C1);
    const float  InvDet = 1.0 / dot(C0, R0);

    FTransform3x4 Out;
    Out.Row0 = float4(R0 * InvDet, -dot(R0, T) * InvDet);
    Out.Row1 = float4(R1 * InvDet, -dot(R1, T) * InvDet);
    Out.Row2 = float4(R2 * InvDet, -dot(R2, T) * InvDet);
    return Out;
}

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID)
{
    const uint Group   = GroupID.x + GroupID.y * MAX_DISPATCH_AXIS;
    const uint Block   = SpanIsEmpty(&Pass().Blocks) ? Group : (SpanContains(&Pass().Blocks, Group) ? SpanGet(&Pass().Blocks, Group) : 0xFFFFFFFFu);
    if (Block == 0xFFFFFFFFu)
    {
        return;
    }
    const uint SlotIdx = Block * LOCAL_SIZE_X + LocalID.x;
    if (!SpanContains(&Pass().RetainedCullEntries, SlotIdx))
    {
        return;
    }

    const FInstanceCullEntry Cull = SpanGet(&Pass().RetainedCullEntries, SlotIdx);
    if (!Cull.HasFlag(EInstanceFlags::Active) || !Cull.HasFlag(EInstanceFlags::HasGeometry) ||
        !Cull.HasFlag(EInstanceFlags::CastShadow) || !Cull.HasFlag(EInstanceFlags::HasDistanceField) ||
        Cull.HasFlag(EInstanceFlags::Skinned))
    {
        return;
    }

    // Reached through the pass block, since a local copy would put its span pointers in a function variable.
    FSunDFShadowGrid* Grid = &Pass().Grid;
    // A degenerate transform leaves a non-finite sphere, whose cell range would turn into a runaway loop.
    const float4 Sphere = Cull.SphereBounds;
    if (!all(isfinite(Sphere)) || Sphere.w < Grid.MinRadius)
    {
        return;
    }

    // Sun rays are parallel, so a caster can only shade receivers whose sun-space footprint it overlaps.
    const float2 SunXY = float2(dot(Sphere.xyz, Grid.SunRight), dot(Sphere.xyz, Grid.SunUp));
    const float2 CellMin = floor((SunXY - Sphere.w - Grid.Origin) / Grid.CellSize);
    const float2 CellMax = floor((SunXY + Sphere.w - Grid.Origin) / Grid.CellSize);
    if (any(CellMax < 0.0) || any(CellMin >= float2(Grid.Dimension)))
    {
        return;
    }

    FInstanceStatic* Static = SpanAt(&Pass().RetainedStatic, SlotIdx);
    const FDistanceFieldVolume Volume = GetDistanceFieldVolume(Static.MeshletHeader);
    if (!Volume.IsValid() || Volume.MaxDistance <= 0.0 || any(Volume.VolumeSize <= 0.0))
    {
        return;
    }

    const FTransform3x4 LocalToWorld = SpanGet(&Pass().RetainedTransforms, SlotIdx);
    const float3 Scale = float3(length(float3(LocalToWorld.Row0.x, LocalToWorld.Row1.x, LocalToWorld.Row2.x)),
                                length(float3(LocalToWorld.Row0.y, LocalToWorld.Row1.y, LocalToWorld.Row2.y)),
                                length(float3(LocalToWorld.Row0.z, LocalToWorld.Row1.z, LocalToWorld.Row2.z)));
    const float MinScale = min(Scale.x, min(Scale.y, Scale.z));
    if (!(MinScale > 1e-6))
    {
        return;
    }

    uint ObjectIndex;
    InterlockedAdd(SpanAt(&Grid.Counters, kSunDFObjectCounter)[0], 1u, ObjectIndex);
    if (ObjectIndex >= Grid.Objects.Count)
    {
        return;
    }

    FSunDFObject Object;
    Object.WorldToLocal = InverseAffine(LocalToWorld);
    Object.Sphere       = Sphere;
    Object.VolumeMin    = Volume.VolumeMin;
    Object.MaxDistance  = Volume.MaxDistance;
    Object.VolumeSize   = Volume.VolumeSize;
    Object.MinScale     = MinScale;
    Object.TextureIndex = Volume.TextureIndex;
    Object.Flags        = Volume.bTwoSided ? kDistanceFieldTwoSided : 0u;
    Object._Pad0        = 0u;
    Object._Pad1        = 0u;
    SpanSet(&Grid.Objects, ObjectIndex, Object);

    // Clamped after the conversion as well, so no float the conversion mishandles can widen the loop past the grid.
    const int  LastCell = int(Grid.Dimension) - 1;
    const int2 First = clamp(int2(max(CellMin, 0.0)), int2(0, 0), int2(LastCell, LastCell));
    const int2 Last  = clamp(int2(min(CellMax, float2(LastCell, LastCell))), int2(0, 0), int2(LastCell, LastCell));
    for (int Y = First.y; Y <= Last.y; ++Y)
    {
        for (int X = First.x; X <= Last.x; ++X)
        {
            const uint Cell = uint(Y) * Grid.Dimension + uint(X);
            uint Index;
            InterlockedAdd(SpanAt(&Grid.CellCounts, Cell)[0], 1u, Index);
            if (Index < Grid.CellCapacity)
            {
                SpanSet(&Grid.CellItems, Cell * Grid.CellCapacity + Index, ObjectIndex);
            }
        }
    }
}
