#include "Includes/SceneGlobals.slang"
#include "Includes/GroupScan.slang"

// One workgroup scans every (view, draw) bucket, each thread owning a contiguous run.
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

    // Spans so each capacity comes from the allocation it describes.
    GPUSpan<FMeshletDraw>      DrawList;
    GPUSpan<FMeshletBlock>     BlockList;
    GPUSpan<FPreSkinnedVertex> PreSkinArena;
    // The append cursor advances even when the write is dropped, so this must be clamped.
    GPUSpan<FGPUInstance>      VisibleInstances;

    // CullInstances leaves accumulated capacities here; this pass rewrites them as bases and the room each arena actually has.
    GPUSpan<FRenderBucket> Buckets;
    GPUSpan<uint> InstanceCount;
    // Read back kFramesInFlight frames later; entries are demand rather than what fit, so an undersized allocation self-corrects.
    GPUSpan<uint> OutTotals;
    // One thread per (surviving instance, view), sized here because only the GPU knows the survivor count.
    GPUSpan<FDispatchIndirectArguments> OutBlockDispatchArgs;
};
FBuildDrawPrefixArgs* Pass() { return GetPassArgs<FBuildDrawPrefixArgs>(); }

// Monotonic in Offset and never past Capacity, so the scaled windows tile the arena without overlapping.
uint ShareOfArena(uint Offset, uint TotalDemand, uint Capacity)
{
    if (TotalDemand <= Capacity)
    {
        return Offset;
    }
    const float Scale = (float)Capacity / (float)TotalDemand;
    return min((uint)floor((float)Offset * Scale), Capacity);
}

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
        FRenderBucket* Bucket = Pass().Buckets.At(i);
        Local += uint2(Bucket.DrawCapacity, Bucket.BlockCapacity);
    }

    uint2 Demand;
    const uint2 Running = GroupExclusiveScan(Local, ThreadIndex, DRAW_PREFIX_THREADS, Demand);
    uint DrawRunning  = Running.x;
    uint BlockRunning = Running.y;

    for (uint i = First; i < Last; ++i)
    {
        FRenderBucket* Bucket = Pass().Buckets.At(i);

        const uint DrawCount  = Bucket.DrawCapacity;
        const uint BlockCount = Bucket.BlockCapacity;

        // Every bucket keeps a share of a short arena, so a batch drops to coarse LODs instead of vanishing.
        const uint DrawStart  = ShareOfArena(DrawRunning, Demand.x, DrawCapacity);
        const uint DrawEnd    = ShareOfArena(DrawRunning + DrawCount, Demand.x, DrawCapacity);
        const uint BlockStart = ShareOfArena(BlockRunning, Demand.y, BlockCapacity);
        const uint BlockEnd   = ShareOfArena(BlockRunning + BlockCount, Demand.y, BlockCapacity);

        Bucket.DrawDemand    = DrawCount;
        Bucket.BlockDemand   = BlockCount;
        Bucket.DrawBase      = DrawStart;
        Bucket.DrawCapacity  = DrawEnd - DrawStart;
        Bucket.BlockBase     = BlockStart;
        Bucket.BlockCapacity = BlockEnd - BlockStart;

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
    const uint RequestedInstances = Pass().InstanceCount.Get(0u);
    const uint VisibleInstances   = min(RequestedInstances, InstanceCapacity);

    Pass().OutTotals.Set(0u, RequestedInstances);
    Pass().OutTotals.Set(1u, (RequestedInstances > InstanceCapacity) ? 1u : 0u);
    Pass().OutTotals.Set(2u, DrawRunning);
    Pass().OutTotals.Set(3u, (DrawRunning > DrawCapacity) ? 1u : 0u);
    Pass().OutTotals.Set(4u, BlockRunning);
    Pass().OutTotals.Set(5u, (BlockRunning > BlockCapacity) ? 1u : 0u);

    // Demand, since the pre-skin cursor keeps advancing past capacity; it is slot 2 of the instance counters block.
    const uint PreSkinRequested = Pass().InstanceCount.Get(2u);
    Pass().OutTotals.Set(6u, PreSkinRequested);
    Pass().OutTotals.Set(7u, (PreSkinRequested > Pass().PreSkinArena.Count) ? 1u : 0u);

    // Folded across Y exactly as the CPU-side grids are; BuildMeshletBlocks undoes it.
    const uint BlockThreads = VisibleInstances * Pass().NumViews;
    const uint BlockGroups  = (BlockThreads + 63u) / 64u;

    FDispatchIndirectArguments BlockArgs;
    BlockArgs.GroupX = min(BlockGroups, (uint)MAX_DISPATCH_AXIS);
    BlockArgs.GroupY = (BlockGroups + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
    BlockArgs.GroupZ = 1u;
    Pass().OutBlockDispatchArgs.Set(0u, BlockArgs);
}
