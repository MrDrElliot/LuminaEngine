#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/DistanceFieldGrid.slang"

// Bins the casters DistanceFieldGridCull.slang found, once to count each cell and again after DistanceFieldGridScan.slang to scatter.

static const uint LOCAL_SIZE_X = 64u;

// Mirror of Lumina::FDFGridBinArgs.
struct FPushConstants
{
    GPUSpan<uint>    LargeObjects;
    FDFObjectGrid    Grid;
    uint             bScatter;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

void BinCasterCells(FDFObjectGrid* Grid, uint ObjectIndex, uint FirstItem, uint ItemStride, bool bScatter)
{
    const int4 Range = Grid.CellRange(Grid.Objects.At(ObjectIndex).Sphere);
    const uint Width = uint(Range.z - Range.x + 1);
    const uint Cells = Width * uint(Range.w - Range.y + 1);
    for (uint Item = FirstItem; Item < Cells; Item += ItemStride)
    {
        const uint Cell = uint(Range.y + int(Item / Width)) * Grid.Dimension + uint(Range.x + int(Item % Width));
        if (!bScatter)
        {
            InterlockedAdd(Grid.CellCounts.At(Cell)[0], 1u);
            continue;
        }

        // The scan left each cell's end, so stepping it down gives every item its own slot and leaves the cell's first behind.
        uint End;
        InterlockedAdd(Grid.CellCounts.At(Cell)[0], 0xFFFFFFFFu, End);
        if (End - 1u < Grid.CellItems.Count)
        {
            Grid.CellItems.Set(End - 1u, ObjectIndex);
        }
    }
}

// The first groups each share one large caster's cells, and every group after them gives a lane to each other caster.
[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID)
{
    FDFObjectGrid* Grid    = &Pass().Grid;
    const bool bScatter    = Pass().bScatter != 0u;
    const uint LargeGroups = Pass().LargeObjects.Count;
    if (GroupID.x < LargeGroups)
    {
        if (GroupID.x < min(Grid.Counters.Get(kDFGridLargeCounter), LargeGroups))
        {
            BinCasterCells(Grid, Pass().LargeObjects.Get(GroupID.x), LocalID.x, LOCAL_SIZE_X, bScatter);
        }
        return;
    }

    const uint ObjectIndex = (GroupID.x - LargeGroups) * LOCAL_SIZE_X + LocalID.x;
    if (ObjectIndex < min(Grid.Counters.Get(kDFGridObjectCounter), Grid.Objects.Count) &&
        (Grid.Objects.At(ObjectIndex).Flags & kDFGridLargeObject) == 0u)
    {
        BinCasterCells(Grid, ObjectIndex, 0u, 1u, bScatter);
    }
}
