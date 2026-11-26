#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/DistanceFieldGrid.slang"
#include "Includes/WorldDistanceField.slang"
#include "Includes/TerrainHeightfield.slang"

// Writes bricks of the world distance field, either merged from the casters binned over the brick's column and the terrains under it, or cleared to empty space.

static const uint kWorldDFComposite = 1u << 8u;
// More casters than this within one brick's reach keep only the first, which a 4 m brick at the finest level never comes near.
static const uint kMaxBrickCasters = 256u;

// Mirror of Lumina::FWorldDFBrickGPU.
struct FWorldDFBrick
{
    int3 Brick;
    uint LevelAndMode;
};

// Mirror of Lumina::FWorldDFUpdateArgs.
struct FPushConstants
{
    GPUSpan<FWorldDFBrick> Bricks;
    float4                 LevelBoxes[kWorldDFLevels];
    FRWTexture3D           Levels[kWorldDFLevels];
    uint                   _Pad;
    // Binned over the composite level's bricks, one cell per column of bricks.
    FDFObjectGrid          Grid;
    uint                   NumTerrains;
    uint                   _Pad1;
    uint                   _Pad2;
    uint                   _Pad3;
    FTerrainHeightfield    Terrains[kMaxTerrainHeightfields];
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

groupshared uint gsCasters[kMaxBrickCasters];
groupshared uint gsCasterCount;

// Distance from P to the caster's surface in world units, out to the box around its volume where the bake has no samples.
float CasterDistance(FDFObject Object, float3 P)
{
    const float3 Local   = DFObjectLocalPoint(Object, P);
    const float3 Clamped = clamp(Local, Object.VolumeMin, Object.VolumeMin + Object.VolumeSize);
    const float  Raw     = SampleDFObject(Object, Clamped);

    // The bake caps how far it records, so a capped sample says only that the surface is at least that far, never that it is near.
    if (Raw >= Object.MaxDistance * 0.98)
    {
        return 1.0e6;
    }

    float Inside = Raw;
    if ((Object.Flags & kDistanceFieldTwoSided) != 0u)
    {
        Inside -= Object.VoxelSize * 0.5;
    }

    // A row of the inverse is one local axis over its world scale, so the gap to the box is measured in world units on every axis.
    const float3 AxisScale = 1.0 / max(float3(length(Object.WorldToLocal.Row0.xyz), length(Object.WorldToLocal.Row1.xyz), length(Object.WorldToLocal.Row2.xyz)), 1e-6);

    // A scaled plane at local distance d lies d / |n / scale| away in the world, where the smallest scale alone darkens a flattened caster's whole box.
    float InsideScale = Object.MinScale;
    // Inside, the local gradient often runs along a stretched axis and would sink the depth far below the surface, so the smallest scale stays.
    if (Inside > 0.0 && max(AxisScale.x, max(AxisScale.y, AxisScale.z)) > Object.MinScale * 1.01)
    {
        // Centered a voxel inside the volume, since past the outermost texel centers the clamped sampler repeats the border and flattens that axis of the gradient.
        const float  Half     = Object.VoxelSize * 0.5;
        const float3 Center   = clamp(Clamped, Object.VolumeMin + Object.VoxelSize, Object.VolumeMin + Object.VolumeSize - Object.VoxelSize);
        const float3 Gradient = float3(SampleDFObject(Object, Center + float3(Half, 0.0, 0.0)) - SampleDFObject(Object, Center - float3(Half, 0.0, 0.0)),
                                       SampleDFObject(Object, Center + float3(0.0, Half, 0.0)) - SampleDFObject(Object, Center - float3(0.0, Half, 0.0)),
                                       SampleDFObject(Object, Center + float3(0.0, 0.0, Half)) - SampleDFObject(Object, Center - float3(0.0, 0.0, Half)));
        if (dot(Gradient, Gradient) > 1e-12)
        {
            InsideScale = 1.0 / length(normalize(Gradient) / AxisScale);
        }
    }
    return Inside * InsideScale + length((Local - Clamped) * AxisScale);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
    const uint BrickIndex = GroupID.x + GroupID.y * MAX_DISPATCH_AXIS;
    if (!Pass().Bricks.Contains(BrickIndex))
    {
        return;
    }
    const FWorldDFBrick Brick = Pass().Bricks.Get(BrickIndex);
    const uint  Level = Brick.LevelAndMode & 0xFFu;
    const float Voxel = Pass().LevelBoxes[Level].w;
    const float Band  = Voxel * kWorldDFBandVoxels;

    const int3   VoxelIndex = Brick.Brick * int(kWorldDFBrickSize) + int3(LocalID);
    const float3 P          = (float3(VoxelIndex) + 0.5) * Voxel;

    float Distance = Band;
    if ((Brick.LevelAndMode & kWorldDFComposite) != 0u)
    {
        if (LocalIndex == 0u)
        {
            gsCasterCount = 0u;
        }
        GroupMemoryBarrierWithGroupSync();

        FDFObjectGrid* Grid = &Pass().Grid;
        const float  BrickWorld = Voxel * float(kWorldDFBrickSize);
        const float3 BoxMin     = float3(Brick.Brick) * BrickWorld;
        const float3 BoxMax     = BoxMin + BrickWorld;
        const int2   Cell       = Grid.CellAt((BoxMin + BoxMax) * 0.5);
        if (all(Cell >= 0) && all(Cell < int(Grid.Dimension)))
        {
            const uint CellIndex = uint(Cell.y) * Grid.Dimension + uint(Cell.x);
            const uint First     = Grid.CellCounts.Get(CellIndex);
            const uint End       = min(Grid.CellCounts.Get(CellIndex + 1u), Grid.CellItems.Count);
            // The column holds every height, so each caster is kept only when its sphere reaches the brick within the band.
            for (uint Item = First + LocalIndex; Item < End; Item += 512u)
            {
                const uint   ObjectIndex = Grid.CellItems.Get(Item);
                const float4 Sphere      = Grid.Objects.Get(ObjectIndex).Sphere;
                const float3 Outside     = max(BoxMin - Sphere.xyz, 0.0) + max(Sphere.xyz - BoxMax, 0.0);
                if (length(Outside) < Sphere.w + Band + Voxel)
                {
                    uint Slot;
                    InterlockedAdd(gsCasterCount, 1u, Slot);
                    if (Slot < kMaxBrickCasters)
                    {
                        gsCasters[Slot] = ObjectIndex;
                    }
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();

        const uint Count = min(gsCasterCount, kMaxBrickCasters);
        [loop]
        for (uint Index = 0u; Index < Count; ++Index)
        {
            Distance = min(Distance, CasterDistance(Grid.Objects.Get(gsCasters[Index]), P));
        }

        [loop]
        for (uint TerrainIndex = 0u; TerrainIndex < min(Pass().NumTerrains, kMaxTerrainHeightfields); ++TerrainIndex)
        {
            const FTerrainHeightfield Terrain = Pass().Terrains[TerrainIndex];
            if (Terrain.Covers(P.xz))
            {
                // Taken over at least a voxel, since a finer slope would read heightmap noise the level cannot hold.
                const float Step = max(Terrain.Stride, Voxel);
                // Sunk half a step below the triangulated ground it was sampled from, so a pixel on that ground never reads as inside it.
                Distance = min(Distance, Terrain.Distance(P, Step) + 0.5 * Step);
            }
        }
    }

    const uint3 Texel = uint3(((VoxelIndex % int(kWorldDFResolution)) + int(kWorldDFResolution)) % int(kWorldDFResolution));
    Pass().Levels[Level][Texel] = float4(clamp(Distance, -Band, Band), 0.0, 0.0, 0.0);
}
