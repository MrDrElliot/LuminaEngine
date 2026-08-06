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
    // Ceiling on the meshlet work domain, computed CPU-side. BatchMeshletCounts is accumulated by
    // CullInstances and nothing else bounds it, so a corrupt counter would otherwise become an
    // unbounded walk in the early cull. It bounds a LOOP, not a grid: the cull's dispatch size is a
    // CPU constant now, so overshooting this costs iterations rather than losing the device.
    // Still a loose worst case (see the CPU side), which part two of this work replaces with the real
    // per-slot sum -- at which point tripping it means corruption rather than a big scene.
    uint  MaxMeshletWork;
    uint  _Pad0;

    uint* BatchMeshletCounts;      // in:  NumDraws entries, view-independent meshlet work
    uint* ViewDrawCounts;          // in:  NumViews * NumDraws entries, per-(view, draw) emit bound
    uint* InstanceCount;           // in:  [0] = survivor count
    uint* OutViewDrawOffsets;      // out: NumViews * NumDraws exclusive prefix over ViewDrawCounts
    // out: [0] = meshlet work domain (the early cull's stride-loop bound; NOT a dispatch size)
    //      [1] = instance count CLAMPED to capacity -- what downstream passes may safely index
    //      [2] = draw-list entries actually required this frame (feedback sizes the allocation from it)
    //      [3] = set when [2] exceeded DrawListCapacity, i.e. meshlets were dropped
    //      [4] = instance count REQUESTED, uncapped. The append cursor keeps advancing past the capacity
    //            on an overflow frame, so this is the true demand and is what the visible-instance buffer
    //            is sized from. Without it the CPU had to assume every retained slot survives.
    //      [5] = set when [4] exceeded MaxVisibleInstances, i.e. whole instances were dropped
    //      [6]/[7] = deferred meshlets demanded / whether that overflowed, written later by the view culls
    //      [8] = meshlet work domain REQUESTED, before the MaxMeshletWork clamp
    //      [9] = set when [8] exceeded MaxMeshletWork, i.e. the domain was truncated and meshlets were
    //            dropped off its tail. The CPU ceiling that produces MaxMeshletWork is a loose worst case
    //            (retained slots x the densest LOD ANY surface has), so it trips long before this does;
    //            this is the flag that means the truncation actually happened.
    uint* OutTotals;
    // out: {GroupCountX, GroupCountY, GroupCountZ} for instance-prefix scan passes 1 and 3. Safe to
    // derive on the GPU because it comes from VisibleInstances, which is clamped to the CPU-supplied
    // capacity of the buffer those instances live in -- a bound that reflects a real allocation rather
    // than an invented one. The early cull has no such anchor (its domain is an iteration count, not a
    // buffer), which is why its grid is a CPU constant and it reads Totals[0] as a loop bound instead.
    uint* OutScanDispatchArgs;
};
FBuildDrawPrefixArgs* Pass() { return GetPassArgs<FBuildDrawPrefixArgs>(); }

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

    // Clamped before it is used for anything -- the dispatch size, the domain guard in CullMeshlets and
    // the CPU's defer-list feedback all read OutTotals[0], so they stay consistent with each other. A
    // frame that trips this drops meshlets off the tail of the domain; one that doesn't loses the GPU.
    const uint MeshletWorkRequested = MeshletWork;
    MeshletWork = min(MeshletWork, Pass().MaxMeshletWork);

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
    Pass().OutTotals[8] = MeshletWorkRequested;                // pre-clamp domain, the real measured demand
    Pass().OutTotals[9] = (MeshletWorkRequested > Pass().MaxMeshletWork) ? 1u : 0u;

    // Instance-prefix scan domain: one workgroup per block of surviving instances. Uses the LOCAL count,
    // never a re-read of OutTotals[1] -- scan pass 2 derives its own block count from the same value, and
    // if the two ever disagreed pass 2 would fold in block sums pass 1 never wrote.
    const uint ScanBlocks = (VisibleInstances + SCAN_BLOCK - 1u) / SCAN_BLOCK;
    Pass().OutScanDispatchArgs[0] = min(ScanBlocks, MAX_GROUPS_X);
    Pass().OutScanDispatchArgs[1] = (ScanBlocks + MAX_GROUPS_X - 1u) / MAX_GROUPS_X;
    Pass().OutScanDispatchArgs[2] = 1u;
}
