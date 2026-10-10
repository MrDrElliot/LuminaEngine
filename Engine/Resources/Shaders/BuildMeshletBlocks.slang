#include "Includes/AppendBuffer.slang"
#include "Includes/SceneGlobals.slang"
#include "Includes/Culling.slang"

// Reads back the range CullInstances reserved against, so the two cannot disagree on LOD or rejects.

struct FBuildMeshletBlocksArgs
{
    uint            NumViews;
    uint            NumBatches;

    GPUSpan<uint>          InstanceCount;
    // Clamped like BuildDrawPrefix, since the append cursor counts past capacity on overflow.
    GPUSpan<FGPUInstance>  VisibleInstances;
    // (base, count) per (visible instance, view), written by CullInstances at reservation time.
    GPUSpan<uint2>         InstanceViewRanges;
    GPUSpan<FRenderBucket> Buckets;
    GPUSpan<FMeshletBlock> OutBlockList;
    // For the coarsest-LOD retry when a run does not fit its bucket's share.
    GPUSpan<FSurfaceDesc>  SurfaceDescs;

    // MESHLET_BUILD_* below, which says which phase this build serves and whether two-phase buckets split at all.
    uint                   BuildMode;
    // This frame's stamp, so a slot visible last frame holds one less than it.
    uint                   VisibilityTag;
    GPUSpan<uint>          PrevVisibility;
};
FBuildMeshletBlocksArgs* Pass() { return GetPassArgs<FBuildMeshletBlocksArgs>(); }

static const uint MESHLET_BUILD_EARLY         = 0u;
static const uint MESHLET_BUILD_LATE          = 1u;
static const uint MESHLET_BUILD_EARLY_NOSPLIT = 2u;

// Claims Count units only while they fit, so a refused run never moves the counter and everything below it stays written.
bool ClaimWithin(uint* Counter, uint Capacity, uint Count, out uint Old)
{
    Old = 0u;
    uint Current;
    InterlockedAdd(Counter[0], 0u, Current);

    // Bounded, since a run that keeps losing the race on a full bucket would not fit anyway.
    for (uint Attempt = 0u; Attempt < 16u; ++Attempt)
    {
        if (Current > Capacity || Capacity - Current < Count)
        {
            return false;
        }

        uint Seen;
        InterlockedCompareExchange(Counter[0], Current, Current + Count, Seen);
        if (Seen == Current)
        {
            Old = Current;
            return true;
        }
        Current = Seen;
    }
    return false;
}

// A bucket whose whole demand fits takes the plain append, which cannot fail since demand rounds each run's blocks the same way.
bool TryReserveRun(FRenderBucket* Bucket, uint Count, out uint Slot)
{
    Slot = 0u;
    const uint Blocks = (Count + (MESHLET_CULL_GROUP_SIZE - 1u)) / MESHLET_CULL_GROUP_SIZE;

    if (Bucket.DrawDemand <= Bucket.DrawCapacity && Bucket.BlockDemand <= Bucket.BlockCapacity)
    {
        uint OldBlock;
        InterlockedAdd(Bucket.BlockCursor, Blocks, OldBlock);
        Slot = Bucket.BlockBase + OldBlock;
        return true;
    }

    // Draw budget a refused block claim already took stays spent, which only makes later runs in this short bucket more conservative.
    uint OldDraw;
    uint OldBlock;
    if (!ClaimWithin(&Bucket.DrawBudgetCursor, Bucket.DrawCapacity, Count, OldDraw)
        || !ClaimWithin(&Bucket.BlockCursor, Bucket.BlockCapacity, Blocks, OldBlock))
    {
        return false;
    }

    Slot = Bucket.BlockBase + OldBlock;
    return true;
}

// The LOD after the one Current starts at, empty when there is none or the instance cannot re-select.
uint2 NextCoarserRange(FGPUInstance* Inst, uint2 Current)
{
    const uint DescIndex = Inst.SurfaceDescIndex;
    if (DescIndex == kNoSurfaceDesc || !Pass().SurfaceDescs.Contains(DescIndex))
    {
        return uint2(0u, 0u);
    }
    const FSurfaceDesc Desc = Pass().SurfaceDescs.Get(DescIndex);
    const uint NumLODs = min(Desc.NumLODs, (uint)MESHLET_MAX_LODS);
    if (NumLODs == 0u)
    {
        return uint2(0u, 0u);
    }

    uint Pick = NumLODs;
    for (uint Lod = 0u; Lod + 1u < NumLODs; ++Lod)
    {
        if (Desc.LODMeshletOffset[Lod] == Current.x)
        {
            Pick = Lod + 1u;
            break;
        }
    }
    if (Pick >= NumLODs)
    {
        return uint2(0u, 0u);
    }

    const uint Base = Desc.LODMeshletOffset[Pick];
    if (Base >= Inst.MeshletTotalCount)
    {
        return uint2(0u, 0u);
    }
    return uint2(Base, min(Desc.LODMeshletCount[Pick], Inst.MeshletTotalCount - Base));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 ThreadID : SV_DispatchThreadID)
{
    const uint NumViews         = max(Pass().NumViews, 1u);
    const uint VisibleInstances = min(Pass().InstanceCount.Get(0u), Pass().VisibleInstances.Count);

    // Undoes the Y fold the dispatch applies past MAX_DISPATCH_AXIS groups.
    const uint Flat       = ThreadID.x + ThreadID.y * MAX_DISPATCH_AXIS * 64u;
    const uint InstanceID = Flat / NumViews;
    const uint ViewIndex  = Flat % NumViews;

    if (InstanceID >= VisibleInstances || ViewIndex >= Pass().NumViews)
    {
        return;
    }

    // Read rather than re-derived, already clamped, and zero when the pair is invisible or out of range.
    uint2 Range = Pass().InstanceViewRanges.Get(Flat);
    if (Range.y == 0u)
    {
        return;
    }

    FGPUInstance* Inst = Instances() + InstanceID;

    // Only for the bucket index; the reject itself already happened where the range was decided.
    const uint DrawID = FGPUInstance::GetDrawID(Inst);
    if (DrawID >= Pass().NumBatches)
    {
        return;
    }

    FRenderBucket* Bucket = Pass().Buckets.At(ViewIndex * Pass().NumBatches + DrawID);

    // A split bucket builds last frame's visible set now and its late blocks after the pyramid, so a hidden instance gets none.
    const bool bSplit = Pass().BuildMode != MESHLET_BUILD_EARLY_NOSPLIT
                     && CullViewHasFlag(ViewIndex, CULL_VIEW_FLAG_MESHLET_HIZ)
                     && BucketFitsDemand(Bucket.DrawDemand, Bucket.DrawCapacity, Bucket.BlockDemand, Bucket.BlockCapacity);
    const uint RetainedSlot = Inst.RetainedSlot;
    const bool bWasVisible  = bSplit && Pass().PrevVisibility.Contains(RetainedSlot)
                           && Pass().PrevVisibility.Get(RetainedSlot) == (Pass().VisibilityTag - 1u);

    if (Pass().BuildMode == MESHLET_BUILD_LATE)
    {
        if (!bSplit)
        {
            return;
        }

        // The same instance test the late meshlet cull applies, so what it rejects here it would have rejected there.
        bool bOccluded = false;
        if (!FGPUInstance::HasFlag(Inst, EInstanceFlags::NoCull))
        {
            const float4 Sphere = loadAligned<16>(&Inst.SphereBounds);
            const FCullSphereProjection Proj = ProjectCullSphere(Sphere.xyz, Sphere.w);
            bOccluded = Proj.bValid && IsHiZOccluded(Proj);
        }
        if (bOccluded)
        {
            return;
        }

        // Early-drawn instances come back too, since a sphere-level flag lets coarse pyramid tiles keep whole trees early for good.
        const uint BackBlocks = (Range.y + (MESHLET_CULL_GROUP_SIZE - 1u)) / MESHLET_CULL_GROUP_SIZE;
        uint OldBack;
        InterlockedAdd(Bucket.BlockBackCount, BackBlocks, OldBack);

        // Front and back together never pass the demand this bucket fits, so this only guards against a broken reservation.
        const uint Front = min(Bucket.BlockCursor, Bucket.BlockCapacity);
        if (OldBack + BackBlocks > Bucket.BlockCapacity - Front)
        {
            return;
        }

        const uint BackSlot = Bucket.BlockBase + Bucket.BlockCapacity - OldBack - BackBlocks;
        for (uint b = 0u; b < BackBlocks; ++b)
        {
            const uint First = b * MESHLET_CULL_GROUP_SIZE;

            FMeshletBlock Block;
            Block.InstanceID         = InstanceID;
            Block.PackedBaseAndCount = FMeshletBlock::Pack(Range.x + First, min(MESHLET_CULL_GROUP_SIZE, Range.y - First));
            Pass().OutBlockList.Set(BackSlot + b, Block);
        }
        return;
    }

    if (bSplit && !bWasVisible)
    {
        return;
    }

    // Only an instance that does not fit steps down, one LOD at a time, so a short frame degrades the overflow and nothing else.
    uint Slot;
    if (!TryReserveRun(Bucket, Range.y, Slot))
    {
        bool bFitted = false;
        for (uint Step = 0u; Step < (uint)MESHLET_MAX_LODS && !bFitted; ++Step)
        {
            const uint2 Coarser = NextCoarserRange(Inst, Range);
            if (Coarser.y == 0u || Coarser.y >= Range.y)
            {
                break;
            }
            Range   = Coarser;
            bFitted = TryReserveRun(Bucket, Range.y, Slot);
        }
        if (!bFitted)
        {
            return;
        }
    }

    const uint Count  = Range.y;
    const uint Blocks = (Count + (MESHLET_CULL_GROUP_SIZE - 1u)) / MESHLET_CULL_GROUP_SIZE;
    for (uint b = 0u; b < Blocks; ++b)
    {
        const uint First = b * MESHLET_CULL_GROUP_SIZE;

        FMeshletBlock Block;
        Block.InstanceID         = InstanceID;
        Block.PackedBaseAndCount = FMeshletBlock::Pack(Range.x + First,
                                                       min(MESHLET_CULL_GROUP_SIZE, Count - First));
        Pass().OutBlockList.Set(Slot + b, Block);
    }
}
