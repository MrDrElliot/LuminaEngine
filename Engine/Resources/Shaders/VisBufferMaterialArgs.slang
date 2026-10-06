#include "Includes/SceneGlobals.slang"
#include "Includes/GroupScan.slang"
#include "Shared/SharedConstants.h"

// Turns each slot's dense tile and sparse pixel counts into list runs and Y-folded dispatch triples, since 4K overruns one dispatch axis.

#define MATERIAL_ARGS_GROUP 1024u
#define MATERIAL_ARGS_SLOTS_PER_THREAD ((MATERIAL_MAX_SLOTS + MATERIAL_ARGS_GROUP - 1u) / MATERIAL_ARGS_GROUP)

struct FMaterialArgsArgs
{
    GPUSpan<uint> Counts;    // dense tiles per slot, then sparse pixels per slot; half its count is the slot count
    GPUSpan<uint> Offsets;   // first list entry of each run, same two halves
    GPUSpan<uint> Cursors;   // reset here, then advanced by the scatter
    GPUSpan<uint> Args;      // a triple per slot, then the scatter's triple at NumSlots * 2
    GPUSpan<uint> OverflowCount;  // the pairs the classify appended past each tile's inline records
    uint  OverflowCapacity;  // the scatter reads no more overflow pairs than the list holds
    uint  NumTiles;
    uint  _Pad1;
    uint  _Pad2;
};
FMaterialArgsArgs* Pass() { return GetPassArgs<FMaterialArgsArgs>(); }

void WriteDispatch(uint Triple, uint Groups)
{
    SpanSet(&Pass().Args, Triple * 3u + 0u, min(Groups, (uint)MAX_DISPATCH_AXIS));
    SpanSet(&Pass().Args, Triple * 3u + 1u, (Groups + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS);
    SpanSet(&Pass().Args, Triple * 3u + 2u, 1u);
}

[shader("compute")]
[numthreads(MATERIAL_ARGS_GROUP, 1, 1)]
void ComputeMain(uint ThreadIndex : SV_GroupIndex)
{
    const uint NumSlots = min(Pass().Counts.Count / 2u, MATERIAL_MAX_SLOTS);
    const uint First    = ThreadIndex * MATERIAL_ARGS_SLOTS_PER_THREAD;

    uint2 Local = uint2(0u, 0u);
    [unroll]
    for (uint i = 0u; i < MATERIAL_ARGS_SLOTS_PER_THREAD; ++i)
    {
        const uint Slot = First + i;
        const uint2 Work = Slot < NumSlots ? uint2(SpanGet(&Pass().Counts, Slot), SpanGet(&Pass().Counts, NumSlots + Slot)) : uint2(0u, 0u);
        Local += Work;
    }

    uint2 Total;
    uint2 Running = GroupExclusiveScan(Local, ThreadIndex, MATERIAL_ARGS_GROUP, Total);

    if (ThreadIndex == 0u)
    {
        const uint OverflowPairs = min(SpanGet(&Pass().OverflowCount, 0u), Pass().OverflowCapacity);
        WriteDispatch(NumSlots * 2u, (Pass().NumTiles + OverflowPairs + 63u) / 64u);
    }

    [unroll]
    for (uint i = 0u; i < MATERIAL_ARGS_SLOTS_PER_THREAD; ++i)
    {
        const uint Slot = First + i;
        if (Slot >= NumSlots)
        {
            break;
        }

        const uint DenseTiles   = SpanGet(&Pass().Counts, Slot);
        const uint SparsePixels = SpanGet(&Pass().Counts, NumSlots + Slot);
        SpanSet(&Pass().Offsets, Slot, Running.x);
        SpanSet(&Pass().Offsets, NumSlots + Slot, Running.y);
        SpanSet(&Pass().Cursors, Slot, 0u);
        SpanSet(&Pass().Cursors, NumSlots + Slot, 0u);
        // One dispatch per slot covers both, since a second small dispatch costs more in ramp and tail than it saves.
        WriteDispatch(Slot, DenseTiles + (SparsePixels + MATERIAL_PIXEL_GROUP_SIZE - 1u) / MATERIAL_PIXEL_GROUP_SIZE);
        Running += uint2(DenseTiles, SparsePixels);
    }
}
