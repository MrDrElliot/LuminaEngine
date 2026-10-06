#include "Includes/AppendBuffer.slang"
#include "Includes/SceneGlobals.slang"

// One thread per (visible instance, view), appending that pair's blocks into the region CullInstances
// reserved for it. It reserved against a range it WROTE, and this reads that range back, so the two
// cannot disagree -- there is no LOD pick or reject set here to keep in step.

struct FBuildMeshletBlocksArgs
{
    uint            NumViews;
    uint            NumBatches;

    GPUSpan<uint>          InstanceCount;
    // Clamped exactly as BuildDrawPrefix clamps it: the append cursor counts past capacity on overflow.
    GPUSpan<FGPUInstance>  VisibleInstances;
    // (base, count) per (visible instance, view), written by CullInstances at reservation time.
    GPUSpan<uint2>         InstanceViewRanges;
    GPUSpan<FRenderBucket> Buckets;
    GPUSpan<FMeshletBlock> OutBlockList;
    // For the coarsest-LOD retry when a run does not fit its bucket's share.
    GPUSpan<FSurfaceDesc>  SurfaceDescs;
};
FBuildMeshletBlocksArgs* Pass() { return GetPassArgs<FBuildMeshletBlocksArgs>(); }

// Reserves a run's worst-case draws and its blocks together, undoing both when either does not fit.
bool TryReserveRun(FRenderBucket* Bucket, uint Count, out uint Slot)
{
    Slot = 0u;
    const uint Blocks = (Count + (MESHLET_CULL_GROUP_SIZE - 1u)) / MESHLET_CULL_GROUP_SIZE;

    uint OldDraw;
    InterlockedAdd(Bucket.DrawBudgetCursor, Count, OldDraw);
    if (OldDraw > Bucket.DrawCapacity || Bucket.DrawCapacity - OldDraw < Count)
    {
        InterlockedAdd(Bucket.DrawBudgetCursor, 0u - Count);
        return false;
    }

    uint OldBlock;
    InterlockedAdd(Bucket.BlockCursor, Blocks, OldBlock);
    if (OldBlock > Bucket.BlockCapacity || Bucket.BlockCapacity - OldBlock < Blocks)
    {
        InterlockedAdd(Bucket.BlockCursor, 0u - Blocks);
        InterlockedAdd(Bucket.DrawBudgetCursor, 0u - Count);
        return false;
    }

    Slot = Bucket.BlockBase + OldBlock;
    return true;
}

// The LOD after the one Current starts at, empty when there is none or the instance cannot re-select.
uint2 NextCoarserRange(FGPUInstance* Inst, uint2 Current)
{
    const uint DescIndex = Inst.SurfaceDescIndex;
    if (DescIndex == kNoSurfaceDesc || !SpanContains(&Pass().SurfaceDescs, DescIndex))
    {
        return uint2(0u, 0u);
    }
    const FSurfaceDesc Desc = SpanGet(&Pass().SurfaceDescs, DescIndex);
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
    const uint VisibleInstances = min(SpanGet(&Pass().InstanceCount, 0u), Pass().VisibleInstances.Count);

    // Undoes the Y fold the dispatch applies past MAX_DISPATCH_AXIS groups.
    const uint Flat       = ThreadID.x + ThreadID.y * MAX_DISPATCH_AXIS * 64u;
    const uint InstanceID = Flat / NumViews;
    const uint ViewIndex  = Flat % NumViews;

    if (InstanceID >= VisibleInstances || ViewIndex >= Pass().NumViews)
    {
        return;
    }

    // The range CullInstances reserved this pair's region from, read rather than re-derived. That is the
    // whole point: the two used to compute it independently in matching branch order, and a drift
    // between them handed one bucket room nobody used and silently dropped another's newest mesh.
    // Already clamped to the mesh's meshlet total, and zero when the pair is invisible or out of range.
    uint2 Range = SpanGet(&Pass().InstanceViewRanges, Flat);
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

    FRenderBucket* Bucket = SpanAt(&Pass().Buckets, ViewIndex * Pass().NumBatches + DrawID);

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
        SpanSet(&Pass().OutBlockList, Slot + b, Block);
    }
}
