#include "Includes/SceneGlobals.slang"
#include "Shared/SharedConstants.h"

// Deferred material classification, step 2: turn the per-material pixel counts into the start offsets the
// scatter writes at, and build every indirect dispatch the rest of the frame runs on.
//
// One thread, since the argument triples are sequential in the running total anyway.
//
// Grid folding is the load-bearing part: a 4K frame is 8.3M pixels, so a material's group count can exceed
// maxComputeWorkGroupCount[0]. The consumers undo the fold with GroupID.y * MAX_DISPATCH_AXIS + GroupID.x,
// which is the same convention RenderUtils::FoldGroupCount produces CPU-side.

struct FPrefixSumArgs
{
    uint* Counts;      // in:  uint[NumSlots]
    uint* Starts;      // out: first pixel-list index owned by each slot
    uint* Cursors;     // out: seeded to Starts; the scatter bumps them
    uint* Args;        // out: uint[NumSlots * 3], one dispatch triple per slot
    uint* LightArgs;   // out: uint[3], one dispatch over every classified pixel
    uint* Total;       // out: uint[1], pixels the deferred lane owns this frame
    uint  NumSlots;
    uint  _Pad0;
};
FPrefixSumArgs* Pass() { return GetPassArgs<FPrefixSumArgs>(); }

void WriteDispatchArgs(uint* Args, uint Base, uint ThreadCount)
{
    const uint Groups = (ThreadCount + MATERIAL_PIXEL_GROUP_SIZE - 1u) / MATERIAL_PIXEL_GROUP_SIZE;
    Args[Base + 0u] = min(Groups, (uint)MAX_DISPATCH_AXIS);
    Args[Base + 1u] = (Groups + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
    Args[Base + 2u] = 1u;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void ComputeMain()
{
    uint Running = 0u;

    // Exactly the live slots, which is what the block was sized for; a fixed bound would overrun Args.
    for (uint Slot = 0u; Slot < Pass().NumSlots; ++Slot)
    {
        const uint Count = Pass().Counts[Slot];

        Pass().Starts[Slot]  = Running;
        Pass().Cursors[Slot] = Running;
        WriteDispatchArgs(Pass().Args, Slot * 3u, Count);

        Running += Count;
    }

    Pass().Total[0] = Running;
    WriteDispatchArgs(Pass().LightArgs, 0u, Running);
}
