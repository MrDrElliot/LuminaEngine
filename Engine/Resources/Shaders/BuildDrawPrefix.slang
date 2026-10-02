#include "Includes/SceneGlobals.slang"
#include "Includes/GroupScan.slang"

// Turns CullInstances' counters into the view-major per-(view, draw) layout every later stage indexes.
// One workgroup scans every bucket in parallel, each thread owning a contiguous run.
#define DRAW_PREFIX_THREADS 256u

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
[numthreads(DRAW_PREFIX_THREADS, 1, 1)]
void ComputeMain(uint ThreadIndex : SV_GroupIndex)
{
    const uint NumBuckets = Pass().NumViews * Pass().NumDraws;
    const uint PerThread  = (NumBuckets + DRAW_PREFIX_THREADS - 1u) / DRAW_PREFIX_THREADS;
    const uint First      = min(ThreadIndex * PerThread, NumBuckets);
    const uint Last       = min(First + PerThread, NumBuckets);

    const uint DrawCapacity  = Pass().DrawList.Count;
    const uint BlockCapacity = Pass().BlockList.Count;

    uint2 Local = uint2(0u, 0u);
    for (uint i = First; i < Last; ++i)
    {
        FRenderBucket* Bucket = SpanAt(&Pass().Buckets, i);
        Local += uint2(Bucket.DrawCapacity, Bucket.BlockCapacity);
    }

    uint2 Demand;
    const uint2 Running = GroupExclusiveScan(Local, ThreadIndex, DRAW_PREFIX_THREADS, Demand);
    uint DrawRunning  = Running.x;
    uint BlockRunning = Running.y;

    for (uint i = First; i < Last; ++i)
    {
        FRenderBucket* Bucket = SpanAt(&Pass().Buckets, i);

        const uint DrawCount  = Bucket.DrawCapacity;
        const uint BlockCount = Bucket.BlockCapacity;

        // Room left in each allocation, since every appender takes its region's length as its capacity.
        const uint DrawRoom  = (DrawRunning  < DrawCapacity)  ? (DrawCapacity  - DrawRunning)  : 0u;
        const uint BlockRoom = (BlockRunning < BlockCapacity) ? (BlockCapacity - BlockRunning) : 0u;

        // A region that cannot fit gets a zero-length window at the end, and its capacity is published as the room it has.
        Bucket.DrawBase      = min(DrawRunning,  DrawCapacity);
        Bucket.DrawCapacity  = min(DrawCount, DrawRoom);
        Bucket.BlockBase     = min(BlockRunning, BlockCapacity);
        Bucket.BlockCapacity = min(BlockCount, BlockRoom);

        DrawRunning  += DrawCount;
        BlockRunning += BlockCount;
    }

    if (ThreadIndex != 0u)
    {
        return;
    }

    // Demand, from the unclamped counts, since the CPU sizes the next allocation from what was needed.
    DrawRunning  = Demand.x;
    BlockRunning = Demand.y;

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
    const uint BlockThreads = VisibleInstances * Pass().NumViews;
    const uint BlockGroups  = (BlockThreads + 63u) / 64u;

    FDispatchIndirectArguments BlockArgs;
    BlockArgs.GroupX = min(BlockGroups, (uint)MAX_DISPATCH_AXIS);
    BlockArgs.GroupY = (BlockGroups + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
    BlockArgs.GroupZ = 1u;
    SpanSet(&Pass().OutBlockDispatchArgs, 0u, BlockArgs);
}
