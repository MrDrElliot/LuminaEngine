#include "Includes/SceneGlobals.slang"

// Turns CullInstances' counters into the draw-argument layout the CPU merge used to own.
//
// One workgroup, one serial scan. A draw is a PSO bucket rather than a geometry bucket, so NumDraws is the
// scene's distinct material/blend-state count -- tens, not thousands -- and NumViews is bounded by
// GMaxCullViews. NumViews * NumDraws is therefore in the hundreds; a hierarchical scan would cost more in
// launch overhead than it saves.
//
// The draw list is laid out PER (view, draw), each region sized to exactly what that pair can emit.
// It used to be NumViews equal slices of the worst-case global total, which meant the camera got 1/NumViews
// of the buffer while shadow cascades sat on space they could never use, and the whole allocation was
// NumViews times larger than the real requirement. Because each region is now an exact upper bound, a
// (view, draw) pair can no longer overflow into its neighbour -- only the whole allocation coming up short
// can drop anything, and OutTotals[2] is what the CPU sizes it from.

struct FBuildDrawPrefixArgs
{
    uint  NumViews;
    uint  NumDraws;
    // Capacity of the visible-instance buffer. CullInstances' append cursor keeps counting past it on an
    // overflow frame (the atomic advances even when the write is dropped), so the count published here
    // must be clamped or the prefix scan and the meshlet cull index off the end.
    uint  MaxVisibleInstances;
    // Entries the meshlet draw list can hold. Regions past this are clamped to zero length rather than
    // being handed offsets outside the allocation.
    uint  DrawListCapacity;

    uint* BatchMeshletCounts;      // in:  NumDraws entries, view-independent meshlet work
    uint* ViewDrawCounts;          // in:  NumViews * NumDraws entries, per-(view, draw) emit bound
    uint* InstanceCount;           // in:  [0] = survivor count
    uint* OutViewDrawOffsets;      // out: NumViews * NumDraws exclusive prefix over ViewDrawCounts
    // out: [0] = meshlet work domain (the early cull's dispatch size)
    //      [1] = instance count CLAMPED to capacity -- what downstream passes may safely index
    //      [2] = draw-list entries actually required this frame (feedback sizes the allocation from it)
    //      [3] = set when [2] exceeded DrawListCapacity, i.e. meshlets were dropped
    //      [4] = instance count REQUESTED, uncapped. The append cursor keeps advancing past the capacity
    //            on an overflow frame, so this is the true demand and is what the visible-instance buffer
    //            is sized from. Without it the CPU had to assume every retained slot survives.
    //      [5] = set when [4] exceeded MaxVisibleInstances, i.e. whole instances were dropped
    uint* OutTotals;
    uint* OutCullDispatchArgs;     // out: {GroupCountX, GroupCountY, GroupCountZ} for the early cull
    uint* OutScanDispatchArgs;     // out: same, for instance-prefix scan passes 1 and 3
};
FBuildDrawPrefixArgs* Pass() { return GetPassArgs<FBuildDrawPrefixArgs>(); }

// Must match CullMeshlets.slang's LOCAL_SIZE_X and its 65535-group Y fold.
#define CULL_LOCAL_SIZE_X 64
#define MAX_GROUPS_X      65535u

// Must match SCAN_BLOCK in Includes/ScanPrefixArgs.slang.
#define SCAN_BLOCK        256u

[shader("compute")]
[numthreads(1, 1, 1)]
void ComputeMain()
{
    const uint NumDraws = Pass().NumDraws;
    const uint NumViews = Pass().NumViews;

    // View-independent meshlet work: the domain the early cull walks, one thread per (instance, meshlet).
    uint MeshletWork = 0u;
    for (uint d = 0u; d < NumDraws; ++d)
    {
        MeshletWork += Pass().BatchMeshletCounts[d];
    }

    // Draw-list layout. Regions are laid out view-major so a view's draws stay contiguous, which keeps the
    // raster's per-batch indirect stride walk sequential.
    const uint Capacity = Pass().DrawListCapacity;
    uint Running = 0u;
    for (uint i = 0u; i < NumViews * NumDraws; ++i)
    {
        // Offsets stay inside the allocation even when the requirement exceeds it: a region that cannot fit
        // is given a zero-length window at the end rather than an offset past it, so EmitMeshlet's bound
        // rejects every write instead of the raster reading past the buffer.
        Pass().OutViewDrawOffsets[i] = min(Running, Capacity);
        Running += Pass().ViewDrawCounts[i];
    }

    // The append cursor advances even when the write is dropped, so this is demand, not what fit.
    const uint RequestedInstances = Pass().InstanceCount[0];
    const uint VisibleInstances   = min(RequestedInstances, Pass().MaxVisibleInstances);

    Pass().OutTotals[0] = MeshletWork;
    Pass().OutTotals[1] = VisibleInstances;
    Pass().OutTotals[2] = Running;                             // what the draw list actually needed
    Pass().OutTotals[3] = (Running > Capacity) ? 1u : 0u;      // meshlets dropped this frame
    Pass().OutTotals[4] = RequestedInstances;                  // what the instance buffer actually needed
    Pass().OutTotals[5] = (RequestedInstances > Pass().MaxVisibleInstances) ? 1u : 0u;

    // Early-cull domain: one thread per (instance, meshlet) pair, folded into Y past the 65535 cap exactly
    // as the CPU dispatch did.
    const uint Groups = (MeshletWork + CULL_LOCAL_SIZE_X - 1u) / CULL_LOCAL_SIZE_X;
    Pass().OutCullDispatchArgs[0] = min(Groups, MAX_GROUPS_X);
    Pass().OutCullDispatchArgs[1] = (Groups + MAX_GROUPS_X - 1u) / MAX_GROUPS_X;
    Pass().OutCullDispatchArgs[2] = 1u;

    // Instance-prefix scan domain: one workgroup per block of surviving instances. Uses the LOCAL count,
    // never a re-read of OutTotals[1] -- scan pass 2 derives its own block count from the same value, and
    // if the two ever disagreed pass 2 would fold in block sums pass 1 never wrote.
    const uint ScanBlocks = (VisibleInstances + SCAN_BLOCK - 1u) / SCAN_BLOCK;
    Pass().OutScanDispatchArgs[0] = min(ScanBlocks, MAX_GROUPS_X);
    Pass().OutScanDispatchArgs[1] = (ScanBlocks + MAX_GROUPS_X - 1u) / MAX_GROUPS_X;
    Pass().OutScanDispatchArgs[2] = 1u;
}
