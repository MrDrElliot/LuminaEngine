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

    // 1 leaves shadow levels for a later build, 2 builds only them, dropping instances that land in no page this frame needs.
    uint                   bDeferShadows;
    uint                   _Pad0;
    uint                   _Pad1;
    uint                   _Pad2;
};
FBuildMeshletBlocksArgs* Pass() { return GetPassArgs<FBuildMeshletBlocksArgs>(); }

static const uint MESHLET_BUILD_EARLY         = 0u;
static const uint MESHLET_BUILD_LATE          = 1u;
static const uint MESHLET_BUILD_EARLY_NOSPLIT = 2u;

// A bucket whose whole demand fits takes the plain append, which cannot fail since demand rounds each run's blocks the same way.
bool TryReserveRun(uint BucketIndex, FRenderBucket* Bucket, uint Blocks, uint DrawCharge, out uint Slot)
{
    Slot = 0u;

    if (Bucket.DrawDemand <= Bucket.DrawCapacity && Bucket.BlockDemand <= Bucket.BlockCapacity)
    {
        Slot = Bucket.BlockBase + WaveInterlockedAddKeyed(BucketIndex, &Bucket.BlockCursor, Blocks);
        return true;
    }

    // Draw budget a refused block claim already took stays spent, which only makes later runs in this short bucket more conservative.
    uint OldDraw;
    uint OldBlock = 0u;
    const bool bDrawClaimed = WaveClaimWithinKeyed(BucketIndex, &Bucket.DrawBudgetCursor, Bucket.DrawCapacity, DrawCharge, OldDraw);
    bool bBlockClaimed = false;
    if (bDrawClaimed)
    {
        bBlockClaimed = WaveClaimWithinKeyed(BucketIndex, &Bucket.BlockCursor, Bucket.BlockCapacity, Blocks, OldBlock);
    }
    if (!bBlockClaimed)
    {
        return false;
    }

    Slot = Bucket.BlockBase + OldBlock;
    return true;
}

// Ranges of a cluster mesh split on runs, so MeshletCull can drop a block whole from its run's bounds.
bool UsesMeshletRuns(FGPUInstance* Inst)
{
    const uint DescIndex = Inst.SurfaceDescIndex;
    return !FGPUInstance::HasFlag(Inst, EInstanceFlags::Skinned) && DescIndex != kNoSurfaceDesc && Pass().SurfaceDescs.Contains(DescIndex);
}

void WriteBlocks(uint InstanceID, uint2 Range, bool bRuns, uint ForceBits, uint Slot)
{
    const uint Blocks = RangeBlockCount(Range, bRuns);
    for (uint b = 0u; b < Blocks; ++b)
    {
        const uint2 Part = RangeBlock(Range, b, bRuns);
        FMeshletBlock Block;
        Block.InstanceID         = InstanceID;
        Block.PackedBaseAndCount = FMeshletBlock::Pack(Part.x, Part.y) | ForceBits;
        Pass().OutBlockList.Set(Slot + b, Block);
    }
}

// What CullInstances charged the bucket for this range, the meshlets its cut can draw rather than every slot it spans, so a short bucket charges the same.
uint ViewDrawCharge(FGPUInstance* Inst, uint ViewIndex, uint Count)
{
    // A skinned range is the camera's pick, charged and kept whole.
    const uint DescIndex = Inst.SurfaceDescIndex;
    if (FGPUInstance::HasFlag(Inst, EInstanceFlags::Skinned) || DescIndex == kNoSurfaceDesc || !Pass().SurfaceDescs.Contains(DescIndex))
    {
        return Count;
    }
    const FSurfaceDesc Desc = Pass().SurfaceDescs.Get(DescIndex);
    uint Bound = Count;
    SelectViewMeshletRange(Desc, CullViews().GetAligned(ViewIndex), loadAligned<16>(&Inst.SphereBounds), TransformMaxScale(Inst.Transform), Bound);
    return min(Bound, Count);
}

// The LOD after the one Current starts at, empty when there is none or the instance cannot re-select.
uint2 NextCoarserRange(FGPUInstance* Inst, uint2 Current, inout bool bForceAll)
{
    // A skinned range is the camera's pick, charged and kept whole.
    const uint DescIndex = Inst.SurfaceDescIndex;
    if (FGPUInstance::HasFlag(Inst, EInstanceFlags::Skinned) || DescIndex == kNoSurfaceDesc || !Pass().SurfaceDescs.Contains(DescIndex))
    {
        return uint2(0u, 0u);
    }
    const FSurfaceDesc Desc = Pass().SurfaceDescs.Get(DescIndex);
    const uint NumLODs = min(Desc.NumLODs, (uint)MESHLET_MAX_LODS);
    if (NumLODs == 0u)
    {
        return uint2(0u, 0u);
    }

    // A range drops to the coarsest slot it already spans, then one slot up, each drawn whole since the cut no longer holds.
    uint Pick = NumLODs;
    const uint End = Current.x + Current.y;
    for (uint Slot = 0u; Slot < NumLODs; ++Slot)
    {
        if (Desc.LODMeshletOffset[Slot] + Desc.LODMeshletCount[Slot] == End)
        {
            Pick = (bForceAll || Desc.LODMeshletOffset[Slot] == Current.x) ? Slot + 1u : Slot;
            break;
        }
    }
    bForceAll = true;
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

    const bool bDeferredShadow = Pass().bDeferShadows != 0u && IsDeferredShadowView(ViewIndex);
    if ((Pass().bDeferShadows == 2u) != bDeferredShadow)
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

    if (Pass().bDeferShadows == 2u && !FGPUInstance::HasFlag(Inst, EInstanceFlags::NoCull))
    {
        const float4 Sphere = loadAligned<16>(&Inst.SphereBounds);
        if (IsShadowLevelCasterCulled(CullViews().GetAligned(ViewIndex), Sphere.xyz, Sphere.w))
        {
            return;
        }
    }

    // Only for the bucket index; the reject itself already happened where the range was decided.
    const uint DrawID = FGPUInstance::GetDrawID(Inst);
    if (DrawID >= Pass().NumBatches)
    {
        return;
    }

    const uint     BucketIndex = ViewIndex * Pass().NumBatches + DrawID;
    FRenderBucket* Bucket      = Pass().Buckets.At(BucketIndex);

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
        const bool bRuns = UsesMeshletRuns(Inst);
        const uint BackBlocks = RangeBlockCount(Range, bRuns);
        const uint OldBack = WaveInterlockedAddKeyed(BucketIndex, &Bucket.BlockBackCount, BackBlocks);

        // Front and back together never pass the demand this bucket fits, so this only guards against a broken reservation.
        const uint Front = min(Bucket.BlockCursor, Bucket.BlockCapacity);
        if (OldBack + BackBlocks > Bucket.BlockCapacity - Front)
        {
            return;
        }

        WriteBlocks(InstanceID, Range, bRuns, 0u, Bucket.BlockBase + Bucket.BlockCapacity - OldBack - BackBlocks);
        return;
    }

    if (bSplit && !bWasVisible)
    {
        return;
    }

    // Only an instance that does not fit steps down, one LOD at a time, so a short frame degrades the overflow and nothing else.
    const bool bRuns = UsesMeshletRuns(Inst);
    bool bForceAll = false;
    uint Blocks = RangeBlockCount(Range, bRuns);
    uint Slot;
    if (!TryReserveRun(BucketIndex, Bucket, Blocks, ViewDrawCharge(Inst, ViewIndex, Range.y), Slot))
    {
        bool bFitted = false;
        for (uint Step = 0u; Step < (uint)MESHLET_MAX_LODS && !bFitted; ++Step)
        {
            const uint2 Coarser = NextCoarserRange(Inst, Range, bForceAll);
            if (Coarser.y == 0u || Coarser.y >= Range.y)
            {
                break;
            }
            Range   = Coarser;
            Blocks  = RangeBlockCount(Range, bRuns);
            bFitted = TryReserveRun(BucketIndex, Bucket, Blocks, Range.y, Slot);
        }
        if (!bFitted)
        {
            return;
        }
    }

    WriteBlocks(InstanceID, Range, bRuns, bForceAll ? MESHLET_BLOCK_FORCE_ALL_BIT : 0u, Slot);
}
