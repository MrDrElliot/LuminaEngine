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

// Monotonic in Offset and never past Capacity, so the scaled windows tile the arena without overlapping, and spare room is shared out the same way.
uint ShareOfArena(uint Offset, uint TotalDemand, uint Capacity)
{
    if (TotalDemand == 0u)
    {
        return 0u;
    }
    return (uint)min(uint64_t(Offset) * Capacity / TotalDemand, uint64_t(Capacity));
}

// Where the last view that writes visibility IDs ends, since only those slots have to fit the ID range.
groupshared uint gsVisIDEnd;

bool IsShadowOnlyBucket(uint BucketIndex)
{
    return CullViewHasFlag(BucketIndex / Pass().NumDraws, CULL_VIEW_FLAG_CAST_SHADOW_ONLY);
}

// Views that write visibility IDs take their whole demand first, since a skinned mesh has no coarser LOD and its share of a short list would drop it from the frame.
struct FArenaTiers
{
    uint SceneDemand;
    uint SceneCapacity;
    uint ShadowDemand;
    uint ShadowCapacity;

    uint Position(uint SceneRunning, uint ShadowRunning)
    {
        return ShareOfArena(SceneRunning, SceneDemand, SceneCapacity) + ShareOfArena(ShadowRunning, ShadowDemand, ShadowCapacity);
    }
};

FArenaTiers MakeArenaTiers(uint SceneDemand, uint ShadowDemand, uint Capacity)
{
    FArenaTiers Tiers;
    Tiers.SceneDemand    = SceneDemand;
    Tiers.SceneCapacity  = min(SceneDemand, Capacity);
    Tiers.ShadowDemand   = ShadowDemand;
    Tiers.ShadowCapacity = Capacity - Tiers.SceneCapacity;
    return Tiers;
}

[shader("compute")]
[numthreads(DRAW_PREFIX_THREADS, 1, 1)]
void ComputeMain(uint ThreadIndex : SV_GroupIndex)
{
    if (ThreadIndex == 0u)
    {
        gsVisIDEnd = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint NumBuckets = Pass().NumViews * Pass().NumDraws;
    const uint PerThread  = (NumBuckets + DRAW_PREFIX_THREADS - 1u) / DRAW_PREFIX_THREADS;
    const uint First      = min(ThreadIndex * PerThread, NumBuckets);
    const uint Last       = min(First + PerThread, NumBuckets);

    const uint DrawCapacity  = Pass().DrawList.Count;
    const uint BlockCapacity = Pass().BlockList.Count;

    uint2 LocalScene  = uint2(0u, 0u);
    uint2 LocalShadow = uint2(0u, 0u);
    for (uint i = First; i < Last; ++i)
    {
        FRenderBucket* Bucket = Pass().Buckets.At(i);
        const uint2 Reserved = uint2(Bucket.DrawCapacity, Bucket.BlockCapacity);
        if (IsShadowOnlyBucket(i))
        {
            LocalShadow += Reserved;
        }
        else
        {
            LocalScene += Reserved;
        }
    }

    uint2 SceneDemand;
    uint2 SceneRunning = GroupExclusiveScan(LocalScene, ThreadIndex, DRAW_PREFIX_THREADS, SceneDemand);
    // The scan's wave totals are shared, so every thread finishes reading them before the second scan writes.
    GroupMemoryBarrierWithGroupSync();
    uint2 ShadowDemand;
    uint2 ShadowRunning = GroupExclusiveScan(LocalShadow, ThreadIndex, DRAW_PREFIX_THREADS, ShadowDemand);

    const uint2 Demand = SceneDemand + ShadowDemand;
    const FArenaTiers DrawTiers  = MakeArenaTiers(SceneDemand.x, ShadowDemand.x, DrawCapacity);
    const FArenaTiers BlockTiers = MakeArenaTiers(SceneDemand.y, ShadowDemand.y, BlockCapacity);

    uint VisIDEnd = 0u;
    for (uint i = First; i < Last; ++i)
    {
        FRenderBucket* Bucket = Pass().Buckets.At(i);

        const uint DrawCount   = Bucket.DrawCapacity;
        const uint BlockCount  = Bucket.BlockCapacity;
        const bool bShadowOnly = IsShadowOnlyBucket(i);
        if (!bShadowOnly)
        {
            VisIDEnd = max(VisIDEnd, SceneRunning.x + ShadowRunning.x + DrawCount);
        }

        const uint2 SceneEnd  = SceneRunning + (bShadowOnly ? uint2(0u, 0u) : uint2(DrawCount, BlockCount));
        const uint2 ShadowEnd = ShadowRunning + (bShadowOnly ? uint2(DrawCount, BlockCount) : uint2(0u, 0u));

        // Every bucket keeps a share of its tier, so a short arena drops static batches to coarse LODs instead of vanishing.
        const uint DrawStart  = DrawTiers.Position(SceneRunning.x, ShadowRunning.x);
        const uint DrawEnd    = DrawTiers.Position(SceneEnd.x, ShadowEnd.x);
        const uint BlockStart = BlockTiers.Position(SceneRunning.y, ShadowRunning.y);
        const uint BlockEnd   = BlockTiers.Position(SceneEnd.y, ShadowEnd.y);

        Bucket.DrawDemand    = DrawCount;
        Bucket.BlockDemand   = BlockCount;
        Bucket.DrawBase      = DrawStart;
        Bucket.DrawCapacity  = DrawEnd - DrawStart;
        Bucket.BlockBase     = BlockStart;
        Bucket.BlockCapacity = BlockEnd - BlockStart;

        SceneRunning  = SceneEnd;
        ShadowRunning = ShadowEnd;
    }

    InterlockedMax(gsVisIDEnd, VisIDEnd);
    GroupMemoryBarrierWithGroupSync();
    if (ThreadIndex != 0u)
    {
        return;
    }
    Pass().OutTotals.Set(9u, gsVisIDEnd);

    // Demand, from the unclamped counts, since the CPU sizes the next allocation from what was needed.
    const uint DrawRunning  = Demand.x;
    const uint BlockRunning = Demand.y;

    const uint InstanceCapacity   = Pass().VisibleInstances.Count;
    const uint RequestedInstances = Pass().InstanceCount.Get(0u);
    const uint VisibleInstances   = min(RequestedInstances, InstanceCapacity);

    Pass().OutTotals.Set(0u, RequestedInstances);
    Pass().OutTotals.Set(1u, (RequestedInstances > InstanceCapacity) ? 1u : 0u);
    Pass().OutTotals.Set(2u, DrawRunning);
    Pass().OutTotals.Set(3u, (DrawRunning > DrawCapacity) ? 1u : 0u);
    // The cull args pass raises this to the list a short bucket would have needed, and the CPU sizes from the larger of the two.
    Pass().OutTotals.Set(8u, 0u);
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
