#include "Includes/SceneGlobals.slang"

// Turns CullInstances' counters into the view-major per-(view, draw) layout every later stage indexes.
// One workgroup, one serial scan: NumViews * NumDraws is in the hundreds, so a hierarchical scan would
// cost more in launch overhead than it saves.

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

    // The arenas this pass lays out. Only their counts are read here, but they arrive as spans so each
    // capacity comes from the allocation it describes rather than from a number passed alongside it.
    GPUSpan<FMeshletDraw>      DrawList;
    GPUSpan<FMeshletBlock>     BlockList;
    GPUSpan<FPreSkinnedVertex> PreSkinArena;
    // The append cursor advances even when the write is dropped, so this must be clamped.
    GPUSpan<FGPUInstance>      VisibleInstances;

    // in/out: CullInstances leaves the two capacities accumulated. This pass turns them into bases and
    // rewrites them as the room each arena actually has, which is the bound every appender then uses.
    GPUSpan<FRenderBucket> Buckets;
    GPUSpan<uint> InstanceCount;
    // Read back kFramesInFlight frames later to size the next allocation. Every entry is DEMAND, not
    // what fit, which is what makes an undersized allocation self-correcting.
    //   [0] instances  [1] overflowed   [2] draw entries  [3] overflowed
    //   [4] blocks     [5] overflowed   [6] pre-skin verts [7] overflowed
    GPUSpan<uint> OutTotals;
    // One thread per (surviving instance, view). Sized here because the survivor count only exists
    // on the GPU.
    GPUSpan<FDispatchIndirectArguments> OutBlockDispatchArgs;
};
FBuildDrawPrefixArgs* Pass() { return GetPassArgs<FBuildDrawPrefixArgs>(); }

[shader("compute")]
[numthreads(1, 1, 1)]
void ComputeMain()
{
    const uint NumDraws = Pass().NumDraws;
    const uint NumViews = Pass().NumViews;

    const uint DrawCapacity  = Pass().DrawList.Count;
    const uint BlockCapacity = Pass().BlockList.Count;

    uint DrawRunning  = 0u;
    uint BlockRunning = 0u;

    for (uint v = 0u; v < NumViews; ++v)
    {
        for (uint d = 0u; d < NumDraws; ++d)
        {
            const uint i = v * NumDraws + d;
            FRenderBucket* Bucket = SpanAt(&Pass().Buckets, i);

            const uint DrawCount  = Bucket.DrawCapacity;
            const uint BlockCount = Bucket.BlockCapacity;

            // Room LEFT in each allocation. Clamping the offset alone is not enough: every appender takes
            // its region's LENGTH as its capacity, so an unclamped length writes past the buffer from a
            // base that is itself in range.
            const uint DrawRoom  = (DrawRunning  < DrawCapacity)  ? (DrawCapacity  - DrawRunning)  : 0u;
            const uint BlockRoom = (BlockRunning < BlockCapacity) ? (BlockCapacity - BlockRunning) : 0u;

            const uint Emits  = min(DrawCount, DrawRoom);
            const uint Blocks = min(BlockCount, BlockRoom);

            // A region that cannot fit gets a zero-length window at the end rather than an offset past it.
            // Capacity is published back as the room, not the requirement, so the task grid and the append
            // bound are the same number and a workgroup can never walk a block the builder had no room for.
            Bucket.DrawBase      = min(DrawRunning,  DrawCapacity);
            Bucket.DrawCapacity  = Emits;
            Bucket.BlockBase     = min(BlockRunning, BlockCapacity);
            Bucket.BlockCapacity = Blocks;

            // Demand, from the UNCLAMPED counts. OutTotals is what the CPU sizes the next allocation
            // from, so it has to say what was needed rather than what fit.
            DrawRunning  += DrawCount;
            BlockRunning += BlockCount;
        }
    }

    const uint InstanceCapacity   = Pass().VisibleInstances.Count;
    const uint RequestedInstances = SpanGet(&Pass().InstanceCount, 0u);
    const uint VisibleInstances   = min(RequestedInstances, InstanceCapacity);

    SpanSet(&Pass().OutTotals, 0u, RequestedInstances);
    SpanSet(&Pass().OutTotals, 1u, (RequestedInstances > InstanceCapacity) ? 1u : 0u);
    SpanSet(&Pass().OutTotals, 2u, DrawRunning);
    SpanSet(&Pass().OutTotals, 3u, (DrawRunning > DrawCapacity) ? 1u : 0u);
    SpanSet(&Pass().OutTotals, 4u, BlockRunning);
    SpanSet(&Pass().OutTotals, 5u, (BlockRunning > BlockCapacity) ? 1u : 0u);

    // CullInstances' pre-skin cursor keeps advancing past capacity, so this is DEMAND -- the number the CPU
    // grows the buffer to, not the number that fit. Slot 2 of the same counters block the instance cursor
    // lives in.
    const uint PreSkinRequested = SpanGet(&Pass().InstanceCount, 2u);
    SpanSet(&Pass().OutTotals, 6u, PreSkinRequested);
    SpanSet(&Pass().OutTotals, 7u, (PreSkinRequested > Pass().PreSkinArena.Count) ? 1u : 0u);

    // Folded across Y exactly as the CPU-side grids are; BuildMeshletBlocks undoes it.
    const uint BlockThreads = VisibleInstances * NumViews;
    const uint BlockGroups  = (BlockThreads + 63u) / 64u;

    FDispatchIndirectArguments BlockArgs;
    BlockArgs.GroupX = min(BlockGroups, (uint)MAX_DISPATCH_AXIS);
    BlockArgs.GroupY = (BlockGroups + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
    BlockArgs.GroupZ = 1u;
    SpanSet(&Pass().OutBlockDispatchArgs, 0u, BlockArgs);
}
