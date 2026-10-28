#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/SunDFShadowCommon.slang"

// Bins the casters SunDFShadowCull.slang found too big for one lane, a whole group sharing each one's cells.

static const uint LOCAL_SIZE_X = 64u;

// Mirror of Lumina::FSunDFShadowBinLargeArgs.
struct FPushConstants
{
    GPUSpan<uint>    LargeObjects;
    FSunDFShadowGrid Grid;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID)
{
    FSunDFShadowGrid* Grid = &Pass().Grid;
    if (GroupID.x >= min(Grid.Counters.Get(kSunDFLargeCounter), Pass().LargeObjects.Count))
    {
        return;
    }

    const uint ObjectIndex = Pass().LargeObjects.Get(GroupID.x);
    const int4 Range = SunDFCellRange(Grid.Objects.Get(ObjectIndex).Sphere, Grid.SunRight, Grid.SunUp, Grid.Origin, Grid.CellSize, Grid.Dimension);
    const uint Width = uint(Range.z - Range.x + 1);
    const uint Cells = Width * uint(Range.w - Range.y + 1);
    for (uint Item = LocalID.x; Item < Cells; Item += LOCAL_SIZE_X)
    {
        const uint Cell = uint(Range.y + int(Item / Width)) * Grid.Dimension + uint(Range.x + int(Item % Width));
        uint Index;
        InterlockedAdd(Grid.CellCounts.At(Cell)[0], 1u, Index);
        if (Index < Grid.CellCapacity)
        {
            Grid.CellItems.Set(Cell * Grid.CellCapacity + Index, ObjectIndex);
        }
    }
}
