#include "Includes/SceneGlobals.slang"

// Turns CullInstances' counters into the per-(view, draw) layout every later stage indexes.
//
// One workgroup, one serial scan. A draw is a PSO bucket rather than a geometry bucket, so NumDraws is the
// scene's distinct material/blend-state count -- tens, not thousands -- and NumViews is bounded by
// GMaxCullViews. NumViews * NumDraws is therefore in the hundreds; a hierarchical scan would cost more in
// launch overhead than it saves.
//
// Three regions come out of here, all view-major so a view's draws stay contiguous:
//   - the DRAW list, sized to exactly what each pair can emit,
//   - the BLOCK list, the same work counted in cull workgroups, which also IS the task dispatch grid,
//   - the DEFER list, which needs no size of its own: a meshlet either emits or defers and never both,
//     so the draw region's length is already an exact upper bound on that pair's defers.
//
// Because each region is an exact upper bound, a pair can no longer overflow into its neighbour -- only
// the whole allocation coming up short can drop anything, and OutTotals[2] is what the CPU sizes it from.

struct FDrawMeshTasksIndirectArguments
{
    uint GroupCountX;
    uint GroupCountY;
    uint GroupCountZ;
};

struct FDispatchIndirectArguments
{
    uint GroupX;
    uint GroupY;
    uint GroupZ;
};

struct FBuildDrawPrefixArgs
{
    uint  NumViews;
    uint  NumDraws;
    // Capacity of the visible-instance buffer. CullInstances' append cursor keeps counting past it on an
    // overflow frame (the atomic advances even when the write is dropped), so the count published here
    // must be clamped or every later pass indexes off the end.
    uint  MaxVisibleInstances;
    // Entries the meshlet draw list can hold. Regions past this are clamped to zero length rather than
    // being handed offsets outside the allocation.
    uint  DrawListCapacity;
    uint  BlockListCapacity;
    uint  DeferListCapacity;

    uint* ViewDrawCounts;          // in:  NumViews * NumDraws entries, per-(view, draw) emit bound
    uint* ViewBlockCounts;         // in:  the same work, counted in cull workgroups
    uint* InstanceCount;           // in:  [0] = survivor count
    uint* OutViewDrawOffsets;      // out: exclusive prefix over ViewDrawCounts
    uint* OutViewBlockOffsets;     // out: exclusive prefix over ViewBlockCounts
    // out: exclusive prefix over ViewDrawCounts, advancing only for views that can defer. A late view
    // consumes its EARLY counterpart's region, so only the deferring views need one.
    uint* OutDeferOffsets;
    // out: {GroupCountX, GroupCountY, GroupCountZ} per (view, draw) -- one task workgroup per block. The
    // late views get theirs from BuildDeferDrawArgs instead, after their pyramid is rebuilt.
    FDrawMeshTasksIndirectArguments* OutTaskDrawArgs;
    // out: the CPU's only view of this frame's demand, read back kFramesInFlight frames later and used
    // to size the next allocation of each list. Every entry is DEMAND, not what fit: the append cursors
    // keep advancing past capacity even when the write is dropped, which is exactly what makes an
    // undersized allocation self-correcting.
    //   [0] instances required   [1] set when [0] exceeded MaxVisibleInstances
    //   [2] draw entries         [3] set when [2] exceeded DrawListCapacity
    //   [4] blocks               [5] set when [4] exceeded BlockListCapacity
    //   [6] defer entries -- no overflow flag; the region layout bounds it exactly (see above)
    uint* OutTotals;
    // out: the grid for BuildMeshletBlocks, one thread per (surviving instance, view). Sized here rather
    // than on the CPU because the survivor count only exists on the GPU -- the CPU can only bound it by
    // the buffer's capacity, which carries the sizing slack plus whatever last frame's demand overshot,
    // and every group past the real count launches only to reject itself.
    FDispatchIndirectArguments* OutBlockDispatchArgs;
};
FBuildDrawPrefixArgs* Pass() { return GetPassArgs<FBuildDrawPrefixArgs>(); }

// A view defers when it tests some Hi-Z in phase 0 -- the camera against its own pyramid, a cascade
// against the cascade atlas's. Everything else (point/spot faces, captures, probes) resolves outright.
// The late views defer nothing: they ARE the re-test.
bool ViewCanDefer(FCullView View)
{
    return !View.HasFlag(CULL_VIEW_FLAG_PHASE_LATE)
        && (View.HasFlag(CULL_VIEW_FLAG_OCCLUSION) || View.HasFlag(CULL_VIEW_FLAG_CASCADE));
}

[shader("compute")]
[numthreads(1, 1, 1)]
void ComputeMain()
{
    const uint NumDraws = Pass().NumDraws;
    const uint NumViews = Pass().NumViews;

    const uint DrawCapacity  = Pass().DrawListCapacity;
    const uint BlockCapacity = Pass().BlockListCapacity;
    const uint DeferCapacity = Pass().DeferListCapacity;

    uint DrawRunning  = 0u;
    uint BlockRunning = 0u;
    uint DeferRunning = 0u;

    for (uint v = 0u; v < NumViews; ++v)
    {
        const bool bDefers = ViewCanDefer(CullViews()[v]);

        for (uint d = 0u; d < NumDraws; ++d)
        {
            const uint i = v * NumDraws + d;

            // Offsets stay inside their allocation even when the requirement exceeds it: a region that
            // cannot fit gets a zero-length window at the end rather than an offset past it, so the
            // shader-side bound rejects every write instead of the raster reading past the buffer.
            Pass().OutViewDrawOffsets[i]  = min(DrawRunning,  DrawCapacity);
            Pass().OutViewBlockOffsets[i] = min(BlockRunning, BlockCapacity);
            Pass().OutDeferOffsets[i]     = min(DeferRunning, DeferCapacity);

            const uint DrawCount  = Pass().ViewDrawCounts[i];
            const uint BlockCount = Pass().ViewBlockCounts[i];

            // One task workgroup per block, clamped to what this region actually got: a short allocation
            // launches fewer workgroups rather than reading past the list.
            const uint Room   = (BlockRunning < BlockCapacity) ? (BlockCapacity - BlockRunning) : 0u;
            const uint Blocks = min(BlockCount, Room);

            FDrawMeshTasksIndirectArguments TaskArgs;
            TaskArgs.GroupCountX = Blocks;
            TaskArgs.GroupCountY = 1u;
            TaskArgs.GroupCountZ = 1u;
            Pass().OutTaskDrawArgs[i] = TaskArgs;

            DrawRunning  += DrawCount;
            BlockRunning += BlockCount;
            if (bDefers)
            {
                DeferRunning += DrawCount;
            }
        }
    }

    const uint RequestedInstances = Pass().InstanceCount[0];
    const uint VisibleInstances   = min(RequestedInstances, Pass().MaxVisibleInstances);

    Pass().OutTotals[0] = RequestedInstances;
    Pass().OutTotals[1] = (RequestedInstances > Pass().MaxVisibleInstances) ? 1u : 0u;
    Pass().OutTotals[2] = DrawRunning;
    Pass().OutTotals[3] = (DrawRunning > DrawCapacity) ? 1u : 0u;
    Pass().OutTotals[4] = BlockRunning;
    Pass().OutTotals[5] = (BlockRunning > BlockCapacity) ? 1u : 0u;
    Pass().OutTotals[6] = DeferRunning;

    // Folded across Y exactly as the CPU-side grids are, and undone in BuildMeshletBlocks with the same
    // shared constant.
    const uint BlockThreads = VisibleInstances * NumViews;
    const uint BlockGroups  = (BlockThreads + 63u) / 64u;

    FDispatchIndirectArguments BlockArgs;
    BlockArgs.GroupX = min(BlockGroups, (uint)MAX_DISPATCH_AXIS);
    BlockArgs.GroupY = (BlockGroups + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
    BlockArgs.GroupZ = 1u;
    Pass().OutBlockDispatchArgs[0] = BlockArgs;
}
