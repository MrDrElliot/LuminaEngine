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
    // Where each view's region of the draw list starts, which its visibility IDs are relative to.
    GPUSpan<uint> OutViewDrawBases;
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

static const uint kTierCamera  = 0u;
static const uint kTierCapture = 1u;
static const uint kTierShadow  = 2u;
static const uint kNumTiers    = 3u;

// Per view, so a view's region and its visibility ID base are one number the raster and every decode share.
groupshared uint gsViewTier[MAX_CULL_VIEWS];
groupshared uint gsViewDemand[MAX_CULL_VIEWS * 2u];
groupshared uint gsViewAllot[MAX_CULL_VIEWS * 2u];
groupshared uint gsViewBase[MAX_CULL_VIEWS * 2u];
// Where each view's first bucket sits in the demand-ordered scan of every bucket.
groupshared uint gsViewStart[MAX_CULL_VIEWS * 2u];
// The water level each tier and arena settles at, every view below it keeping its whole demand.
groupshared uint gsTierLevel[kNumTiers * 2u];
groupshared uint gsTierCapacity[kNumTiers * 2u];
groupshared uint gsTierBase[kNumTiers * 2u];
// Each tier and arena's largest view demand and its whole demand, reduced across the group so no lane walks every view.
groupshared uint     gsTierMax[kNumTiers * 2u];
groupshared uint64_t gsTierWaveTotals[(DRAW_PREFIX_THREADS / 4u) * kNumTiers * 2u];
groupshared uint     gsCameraIDs;

// The camera and its custom depth view, then view captures, then shadow views.
uint ViewTier(uint ViewIndex)
{
    if (CullViewHasFlag(ViewIndex, CULL_VIEW_FLAG_CAST_SHADOW_ONLY))
    {
        return kTierShadow;
    }
    return CullViews().At(ViewIndex).bCaptureView != 0u ? kTierCapture : kTierCamera;
}

// What the tier's views take at water level Level, each the smaller of its demand and the level.
uint TierTakeAtLevel(uint Tier, uint Arena, uint Level, uint NumViews)
{
    uint64_t Taken = 0u;
    for (uint v = 0u; v < NumViews; ++v)
    {
        if (gsViewTier[v] == Tier)
        {
            Taken += min(gsViewDemand[v * 2u + Arena], Level);
        }
    }
    return (uint)min(Taken, uint64_t(0xFFFFFFFFu));
}

// The highest level whose take fits, so small views keep everything and only those above it are trimmed, which is max-min fair.
uint FindTierLevel(uint Tier, uint Arena, uint Capacity, uint NumViews, uint MaxDemand, uint TotalDemand)
{
    // At the largest demand every view keeps all of it, so the take there is the whole demand.
    if (TotalDemand <= Capacity)
    {
        return MaxDemand;
    }

    // Bounded by the largest demand, so the search ends in at most 32 steps and the midpoint never overflows.
    uint Lo = 0u;
    uint Hi = MaxDemand;
    while (Lo < Hi)
    {
        const uint Mid = (uint)((uint64_t(Lo) + uint64_t(Hi) + 1u) >> 1u);
        if (TierTakeAtLevel(Tier, Arena, Mid, NumViews) <= Capacity)
        {
            Lo = Mid;
        }
        else
        {
            Hi = Mid - 1u;
        }
    }
    return Lo;
}

[shader("compute")]
[numthreads(DRAW_PREFIX_THREADS, 1, 1)]
void ComputeMain(uint ThreadIndex : SV_GroupIndex)
{
    const uint NumViews   = min(Pass().NumViews, (uint)MAX_CULL_VIEWS);
    const uint NumDraws   = max(Pass().NumDraws, 1u);
    const uint NumBuckets = NumViews * NumDraws;
    const uint PerThread  = (NumBuckets + DRAW_PREFIX_THREADS - 1u) / DRAW_PREFIX_THREADS;
    const uint First      = min(ThreadIndex * PerThread, NumBuckets);
    const uint Last       = min(First + PerThread, NumBuckets);

    const uint DrawCapacity  = Pass().DrawList.Count;
    const uint BlockCapacity = Pass().BlockList.Count;

    if (ThreadIndex < NumViews)
    {
        gsViewTier[ThreadIndex]            = ViewTier(ThreadIndex);
        gsViewDemand[ThreadIndex * 2u]     = 0u;
        gsViewDemand[ThreadIndex * 2u + 1u] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint2 Local = uint2(0u, 0u);
    for (uint i = First; i < Last; ++i)
    {
        FRenderBucket* Bucket = Pass().Buckets.At(i);
        const uint2 Reserved = uint2(Bucket.DrawCapacity, Bucket.BlockCapacity);
        Local += Reserved;
        InterlockedAdd(gsViewDemand[(i / NumDraws) * 2u], Reserved.x);
        InterlockedAdd(gsViewDemand[(i / NumDraws) * 2u + 1u], Reserved.y);
    }

    // Demand space, every bucket in index order, which a view's own buckets fill contiguously.
    uint2 Demand;
    uint2 Running = GroupExclusiveScan(Local, ThreadIndex, DRAW_PREFIX_THREADS, Demand);
    GroupMemoryBarrierWithGroupSync();

    // One thread per view feeds its tier's largest and whole demand, the sums in 64 bits as the clamped take needs.
    const bool  bView      = ThreadIndex < NumViews;
    const uint  ViewTierId = bView ? gsViewTier[ThreadIndex] : 0u;
    const uint2 ViewDemand = bView ? uint2(gsViewDemand[ThreadIndex * 2u], gsViewDemand[ThreadIndex * 2u + 1u]) : uint2(0u, 0u);
    if (ThreadIndex < kNumTiers * 2u)
    {
        gsTierMax[ThreadIndex] = 0u;
    }
    if (ThreadIndex == 0u)
    {
        gsCameraIDs = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint Wave = ThreadIndex / WaveGetLaneCount();
    [unroll]
    for (uint Slot = 0u; Slot < kNumTiers * 2u; ++Slot)
    {
        const bool     bMine = bView && ViewTierId == (Slot >> 1u);
        const uint     Value = bMine ? ViewDemand[Slot & 1u] : 0u;
        const uint64_t Sum   = WaveActiveSum(uint64_t(Value));
        if (WaveIsFirstLane())
        {
            gsTierWaveTotals[Wave * kNumTiers * 2u + Slot] = Sum;
        }
        if (bMine)
        {
            InterlockedMax(gsTierMax[Slot], Value);
        }
    }
    if (bView && ViewTierId == kTierCamera)
    {
        InterlockedMax(gsCameraIDs, ViewDemand.x);
    }
    GroupMemoryBarrierWithGroupSync();

    // One lane per tier and arena, the camera served first, captures next, and shadows sharing the rest.
    if (ThreadIndex < kNumTiers * 2u)
    {
        const uint NumWaves = (DRAW_PREFIX_THREADS + WaveGetLaneCount() - 1u) / WaveGetLaneCount();
        const uint Arena = ThreadIndex & 1u;
        const uint Tier  = ThreadIndex >> 1u;
        uint64_t Totals[kNumTiers] = { 0u, 0u, 0u };
        for (uint w = 0u; w < NumWaves; ++w)
        {
            [unroll]
            for (uint t = 0u; t < kNumTiers; ++t)
            {
                Totals[t] += gsTierWaveTotals[w * kNumTiers * 2u + t * 2u + Arena];
            }
        }
        const uint Total       = Arena == 0u ? DrawCapacity : BlockCapacity;
        const uint CameraTake  = min((uint)min(Totals[kTierCamera], uint64_t(0xFFFFFFFFu)), Total);
        const uint CaptureTake = min((uint)min(Totals[kTierCapture], uint64_t(0xFFFFFFFFu)), Total - CameraTake);
        const uint Capacity = Tier == kTierCamera ? CameraTake : (Tier == kTierCapture ? CaptureTake : Total - CameraTake - CaptureTake);
        gsTierCapacity[ThreadIndex] = Capacity;
        gsTierBase[ThreadIndex]     = Tier == kTierCamera ? 0u : (Tier == kTierCapture ? CameraTake : CameraTake + CaptureTake);
        gsTierLevel[ThreadIndex]    = FindTierLevel(Tier, Arena, Capacity, NumViews, gsTierMax[ThreadIndex],
                                                    (uint)min(Totals[Tier], uint64_t(0xFFFFFFFFu)));
    }
    GroupMemoryBarrierWithGroupSync();

    // Each tier's views are laid out in view order from the tier's base, and each view's start in demand space is the sum before it.
    const uint2 ViewAllot = bView ? min(ViewDemand, uint2(gsTierLevel[ViewTierId * 2u], gsTierLevel[ViewTierId * 2u + 1u])) : uint2(0u, 0u);
    uint2 ScanTotal;
    const uint2 ViewStart = GroupExclusiveScan(ViewDemand, ThreadIndex, DRAW_PREFIX_THREADS, ScanTotal);
    uint2 TierCursor = uint2(0u, 0u);
    [unroll]
    for (uint t = 0u; t < kNumTiers; ++t)
    {
        GroupMemoryBarrierWithGroupSync();
        const uint2 Cursor = GroupExclusiveScan(ViewTierId == t ? ViewAllot : uint2(0u, 0u), ThreadIndex, DRAW_PREFIX_THREADS, ScanTotal);
        TierCursor = ViewTierId == t ? Cursor : TierCursor;
    }
    if (bView)
    {
        gsViewAllot[ThreadIndex * 2u]      = ViewAllot.x;
        gsViewAllot[ThreadIndex * 2u + 1u] = ViewAllot.y;
        gsViewBase[ThreadIndex * 2u]       = gsTierBase[ViewTierId * 2u] + TierCursor.x;
        gsViewBase[ThreadIndex * 2u + 1u]  = gsTierBase[ViewTierId * 2u + 1u] + TierCursor.y;
        gsViewStart[ThreadIndex * 2u]      = ViewStart.x;
        gsViewStart[ThreadIndex * 2u + 1u] = ViewStart.y;
    }
    GroupMemoryBarrierWithGroupSync();

    if (ThreadIndex < NumViews && Pass().OutViewDrawBases.Contains(ThreadIndex))
    {
        Pass().OutViewDrawBases.Set(ThreadIndex, gsViewBase[ThreadIndex * 2u]);
    }

    for (uint i = First; i < Last; ++i)
    {
        FRenderBucket* Bucket = Pass().Buckets.At(i);
        const uint View = i / NumDraws;

        const uint DrawCount  = Bucket.DrawCapacity;
        const uint BlockCount = Bucket.BlockCapacity;
        const uint2 Start = Running - uint2(gsViewStart[View * 2u], gsViewStart[View * 2u + 1u]);
        const uint2 End   = Start + uint2(DrawCount, BlockCount);

        // Every bucket keeps a share of its view's allotment, so a short arena drops static batches to coarse LODs instead of vanishing.
        const uint DrawDemand  = gsViewDemand[View * 2u];
        const uint BlockDemand = gsViewDemand[View * 2u + 1u];
        const uint DrawStart  = gsViewBase[View * 2u]      + ShareOfArena(Start.x, DrawDemand,  gsViewAllot[View * 2u]);
        const uint DrawEnd    = gsViewBase[View * 2u]      + ShareOfArena(End.x,   DrawDemand,  gsViewAllot[View * 2u]);
        const uint BlockStart = gsViewBase[View * 2u + 1u] + ShareOfArena(Start.y, BlockDemand, gsViewAllot[View * 2u + 1u]);
        const uint BlockEnd   = gsViewBase[View * 2u + 1u] + ShareOfArena(End.y,   BlockDemand, gsViewAllot[View * 2u + 1u]);

        Bucket.DrawDemand    = DrawCount;
        Bucket.BlockDemand   = BlockCount;
        Bucket.DrawBase      = DrawStart;
        Bucket.DrawCapacity  = DrawEnd - DrawStart;
        Bucket.BlockBase     = BlockStart;
        Bucket.BlockCapacity = BlockEnd - BlockStart;

        Running += uint2(DrawCount, BlockCount);
    }

    if (ThreadIndex != 0u)
    {
        return;
    }

    // The camera's largest view, since its IDs are what the shared LOD pressure has to keep inside the range.
    Pass().OutTotals.Set(9u, gsCameraIDs);

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
