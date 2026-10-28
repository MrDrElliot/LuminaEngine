#include "Includes/AppendBuffer.slang"
#include "Includes/MeshletCullCore.slang"
#include "Includes/MeshletGeometry.slang"
#include "Includes/ShadowReceiver.slang"

// The early phase replays last frame's visible meshlets and the late phase tests the rest against this frame's pyramid.

struct FMeshletCullArgs
{
    uint            NumViews;
    // A bucket index is ViewIndex * NumDraws + DrawID, which is how a workgroup recovers its cull view.
    uint            NumDraws;
    uint            Slice;
    // This frame's stamp. A slot was visible last frame iff it holds the previous tag.
    uint            VisibilityTag;
    // Matches the block build, so the late phase reads the back region only when the build put blocks there.
    uint            bSplitPhases;
    // 1 keeps near cascades out of this dispatch, 2 culls only them, against ReceiverMask once the frame's depth is final.
    uint            bDeferCascades;
    uint            ReceiverMaskDim;
    int             SunShadowDataIndex;

    GPUSpan<FRenderBucket> Buckets;
    GPUSpan<FMeshletBlock> BlockList;
    // Read by both dispatches and written by neither, so the partition sees one set of flags. A missing buffer has count 0.
    GPUSpan<uint>          PrevVisibility;
    // Stamped with VisibilityTag by every visible block; identical stores need no atomic and no clear.
    GPUSpan<uint>          OutVisibility;
    // One word per block of an instance's camera LOD. Both phases read it, and only the late phase replaces it.
    GPUSpan<uint>          MeshletVisibility;
    GPUSpan<uint>          ReceiverMask;
    // Word 3 is how many blocks BuildMeshletCullArgs gave each group.
    GPUSpan<uint4>         CullDispatch;
};
FMeshletCullArgs* Pass() { return GetPassArgs<FMeshletCullArgs>(); }

#if MESHLET_CULL_GROUP_SIZE > 32
#error "MESHLET_CULL_GROUP_SIZE must fit one wave, or the wave votes below cover only part of the group."
#endif

static const uint kBlockTwoPhase        = 1u;
static const uint kBlockWasVisible      = 2u;
static const uint kBlockDeferredCascade = 4u;

// A group culls up to MESHLET_CULL_GROUP_SIZE blocks, so a block holding one cube's meshlet no longer idles 31 lanes.
groupshared uint gsBlockArg[MESHLET_CULL_GROUP_SIZE];
groupshared uint gsBlockInstance[MESHLET_CULL_GROUP_SIZE];
groupshared uint gsBlockMeshletBase[MESHLET_CULL_GROUP_SIZE];
groupshared uint gsBlockPrevBits[MESHLET_CULL_GROUP_SIZE];
groupshared uint gsBlockFlags[MESHLET_CULL_GROUP_SIZE];
groupshared uint gsBlockStart[MESHLET_CULL_GROUP_SIZE + 1];
groupshared uint gsBlockVisibleBits[MESHLET_CULL_GROUP_SIZE];

// Inverts the dense prefix BuildMeshletCullArgs writes, since blocks are sparse across the arena.
uint FindBucketForWork(uint WorkIndex, uint NumBuckets)
{
    uint Lo = 0u;
    uint Hi = NumBuckets;
    while (Lo + 1u < Hi)
    {
        const uint Mid = (Lo + Hi) >> 1u;
        if (Pass().Buckets.At(Mid).CullWorkBase <= WorkIndex)
        {
            Lo = Mid;
        }
        else
        {
            Hi = Mid;
        }
    }
    return Lo;
}

// The block whose meshlet range holds Item, the last one starting at or before it.
uint FindBlockForItem(uint Item, uint NumBlocks)
{
    uint Lo = 0u;
    uint Hi = NumBlocks;
    while (Lo + 1u < Hi)
    {
        const uint Mid = (Lo + Hi) >> 1u;
        if (gsBlockStart[Mid] <= Item)
        {
            Lo = Mid;
        }
        else
        {
            Hi = Mid;
        }
    }
    return Lo;
}

// The receiver test against this pass's mask, which only the cascade phase binds.
bool IsShadowUnreceived(uint CascadeIndex, float3 Center, float Radius)
{
    return IsShadowUnreceived(Pass().ReceiverMask, Pass().ReceiverMaskDim, Pass().SunShadowDataIndex, CascadeIndex, Center, Radius);
}

[shader("compute")]
[numthreads(MESHLET_CULL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint ThreadID : SV_GroupIndex)
{
    const uint NumDraws   = max(Pass().NumDraws, 1u);
    const uint NumBuckets = Pass().NumViews * NumDraws;
    const bool bLate      = (Pass().Slice == MESHLET_SLICE_LATE);
    const uint NumBlocks  = clamp(Pass().CullDispatch.IsEmpty() ? 1u : Pass().CullDispatch.Get(0u).w, 1u, (uint)MESHLET_CULL_GROUP_SIZE);

    // Undoes the Y fold BuildMeshletCullArgs applies past MAX_DISPATCH_AXIS groups.
    const uint GroupWork = GroupID.x + GroupID.y * (uint)MAX_DISPATCH_AXIS;

    // Lane b gathers block b's state, then every block's meshlets are shared out across all the lanes.
    uint ArgIdx   = 0u;
    uint Flags    = 0u;
    bool bHasWork = false;
    FMeshletBlock Block = (FMeshletBlock)0;
    if (ThreadID < NumBlocks && NumBuckets > 0u)
    {
        const uint WorkIndex = GroupWork * NumBlocks + ThreadID;
        ArgIdx = FindBucketForWork(WorkIndex, NumBuckets);

        const FRenderBucket Bucket = Pass().Buckets.GetAligned(ArgIdx);
        const uint BlockSlot = WorkIndex - Bucket.CullWorkBase;

        // The late phase of a split bucket culls only the blocks the late build appended at the region's end.
        const bool bBackRegion = bLate && Pass().bSplitPhases != 0u && CullViewHasFlag(ArgIdx / NumDraws, CULL_VIEW_FLAG_MESHLET_HIZ)
                              && BucketFitsDemand(Bucket.DrawDemand, Bucket.DrawCapacity, Bucket.BlockDemand, Bucket.BlockCapacity);

        // Bounded by what was appended, not what CullInstances reserved, since unwritten blocks hold last frame's ring.
        const uint Front         = min(Bucket.BlockCursor, Bucket.BlockCapacity);
        const uint BlocksWritten = bBackRegion ? min(Bucket.BlockBackCount, Bucket.BlockCapacity - Front) : Front;
        const bool bBlockInRange = BlockSlot < BlocksWritten;
        const uint BlockIdx      = bBackRegion ? (Bucket.BlockBase + Bucket.BlockCapacity - BlocksWritten + BlockSlot)
                                               : (Bucket.BlockBase + BlockSlot);

        // Single-phase views belong to the early dispatch, except near cascades waiting on this frame's receiver mask.
        const uint ViewIndex        = ArgIdx / NumDraws;
        const bool bTwoPhase        = CullViewHasFlag(ViewIndex, CULL_VIEW_FLAG_MESHLET_HIZ);
        const bool bDeferredCascade = Pass().bDeferCascades != 0u && CullViewHasFlag(ViewIndex, CULL_VIEW_FLAG_CASCADE)
                                   && !CullViewHasFlag(ViewIndex, CULL_VIEW_FLAG_FAR_CASCADE);
        const bool bCascadePhase    = Pass().bDeferCascades == 2u;
        bHasWork = bBlockInRange && (bCascadePhase ? bDeferredCascade : (!bDeferredCascade && (!bLate || bTwoPhase)));
        if (bHasWork)
        {
            Block = Pass().BlockList.Get(BlockIdx);
        }
        Flags = (bTwoPhase ? kBlockTwoPhase : 0u) | (bDeferredCascade ? kBlockDeferredCascade : 0u);
    }

    FGPUInstance* Inst = Instances() + Block.InstanceID;
    const bool bTwoPhaseWork = bHasWork && (Flags & kBlockTwoPhase) != 0u;

    uint RetainedSlot = 0u;
    if (bTwoPhaseWork)
    {
        RetainedSlot = Inst.RetainedSlot;
        Flags |= Pass().PrevVisibility.Get(RetainedSlot) == (Pass().VisibilityTag - 1u) ? kBlockWasVisible : 0u;
    }

    // A block from the coarser-LOD overflow fallback is not one of the camera range's blocks, so it has no word.
    uint MeshletVisibilityWord = 0u;
    bool bHasMeshletVisibility = false;
    uint MeshletVisibilityBits = ~0u;
    if (bTwoPhaseWork && Inst.MeshletVisibilityBase != kNoMeshletVisibility && Block.MeshletBase >= Inst.SurfaceMeshletOffset)
    {
        const uint RangeOffset = Block.MeshletBase - Inst.SurfaceMeshletOffset;
        if (RangeOffset < Inst.SurfaceMeshletCount && (RangeOffset % MESHLET_CULL_GROUP_SIZE) == 0u)
        {
            MeshletVisibilityWord = Inst.MeshletVisibilityBase + RangeOffset / MESHLET_CULL_GROUP_SIZE;
            bHasMeshletVisibility = Pass().MeshletVisibility.Contains(MeshletVisibilityWord);
            if (bHasMeshletVisibility)
            {
                MeshletVisibilityBits = Pass().MeshletVisibility.Get(MeshletVisibilityWord);
            }
        }
    }

    // Late only, because the early phase is a replay and an instance dropped there is dropped for good.
    bool bInstanceOccluded = false;
    if (bLate && bTwoPhaseWork && !FGPUInstance::HasFlag(Inst, EInstanceFlags::NoCull))
    {
        // The one occlusion test a skinned instance can take, since its sphere is re-fed every frame.
        const float4 Sphere = loadAligned<16>(&Inst.SphereBounds);
        const FCullSphereProjection Proj = ProjectCullSphere(Sphere.xyz, Sphere.w);

        // It leaves every meshlet invisible, so the flag clears for next frame.
        bInstanceOccluded = Proj.bValid && IsHiZOccluded(Proj);
    }

    const uint Meshlets = (bHasWork && !bInstanceOccluded) ? Block.Count : 0u;
    const uint Start    = WavePrefixSum(Meshlets);
    const uint Total    = WaveActiveSum(Meshlets);
    if (ThreadID < NumBlocks)
    {
        gsBlockArg[ThreadID]         = ArgIdx;
        gsBlockInstance[ThreadID]    = Block.InstanceID;
        gsBlockMeshletBase[ThreadID] = Block.MeshletBase;
        gsBlockPrevBits[ThreadID]    = MeshletVisibilityBits;
        gsBlockFlags[ThreadID]       = Flags;
        gsBlockStart[ThreadID]       = Start;
        gsBlockVisibleBits[ThreadID] = 0u;
    }
    if (ThreadID == 0u)
    {
        gsBlockStart[NumBlocks] = Total;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint Base = 0u; Base < Total; Base += MESHLET_CULL_GROUP_SIZE)
    {
        const uint Item = Base + ThreadID;
        bool bKeep        = false;
        uint KeepArg      = 0u;
        uint KeepInstance = 0u;
        uint KeepMeshlet  = 0u;
        if (Item < Total)
        {
            const uint Owner      = FindBlockForItem(Item, NumBlocks);
            const uint Local      = Item - gsBlockStart[Owner];
            const uint OwnerFlags = gsBlockFlags[Owner];
            KeepArg      = gsBlockArg[Owner];
            KeepInstance = gsBlockInstance[Owner];
            KeepMeshlet  = gsBlockMeshletBase[Owner] + Local;

            const bool bTwoPhase          = (OwnerFlags & kBlockTwoPhase) != 0u;
            const bool bWasMeshletVisible = (OwnerFlags & kBlockWasVisible) != 0u && ((gsBlockPrevBits[Owner] >> Local) & 1u) != 0u;

            // Early emits last frame's visible meshlets and late their complement, so each rasterizes once whatever the bits hold.
            const bool bEmitPhase = !bTwoPhase || (bLate != bWasMeshletVisible);

            // Late also tests what early drew, since that test is what writes next frame's flag and bits.
            if (bLate || bEmitPhase)
            {
                const FMeshletCullContext Ctx = MakeMeshletCullContext(Instances() + KeepInstance);
                FMeshletBoundsCache Cache     = MakeMeshletBoundsCache();
                const FCullView View          = CullViews().GetAligned(KeepArg / NumDraws);

                // No Hi-Z in the early replay, since the late phase never re-emits what the early phase drew.
                bool bVisible = TestMeshletAgainstView(Ctx, View, KeepMeshlet, Cache, bLate);
                if (bVisible && (OwnerFlags & kBlockDeferredCascade) != 0u && Ctx.bPosedBounds)
                {
                    bVisible = !IsShadowUnreceived(View.CascadeIndex, Cache.WorldCenter, Cache.WorldRadius);
                }

                bKeep = bEmitPhase && bVisible;
                if (bVisible)
                {
                    InterlockedOr(gsBlockVisibleBits[Owner], 1u << Local);
                }
            }
        }

        // One atomic per bucket the wave's survivors land in, which is nearly always one.
        bool bPending = bKeep;
        while (WaveActiveAnyTrue(bPending))
        {
            const uint Arg   = WaveActiveMin(bPending ? KeepArg : 0xFFFFFFFFu);
            const bool bMine = bPending && KeepArg == Arg;
            FRenderBucket* Bucket = Pass().Buckets.At(Arg);

            // The cursor still advances by the full count past capacity, so the readback keeps reporting demand.
            const uint Kept = WaveActiveCountBits(bMine);
            uint Old = 0u;
            if (bMine && WavePrefixCountBits(bMine) == 0u)
            {
                uint* Cursor = &Bucket.DrawCursor;
                InterlockedAdd(Cursor[0], Kept, Old);
            }
            Old = WaveActiveMax(Old);

            const uint Slot = Old + WavePrefixCountBits(bMine);
            if (bMine && Slot < Bucket.DrawCapacity)
            {
                FMeshletDraw Draw;
                Draw.InstanceID        = KeepInstance;
                Draw.PackedIndexAndTag = FMeshletDraw::Pack(KeepMeshlet, Scene().CullData.MeshletDrawTag);
                MeshletDrawList().Set(Bucket.DrawBase + Slot, Draw);
            }
            bPending = bPending && !bMine;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (!bHasWork || bInstanceOccluded)
    {
        return;
    }
    const uint VisibleMeshlets = gsBlockVisibleBits[ThreadID];

    // This block owns its whole word, so a plain store replaces it.
    if (bLate && bHasMeshletVisibility)
    {
        Pass().MeshletVisibility.Set(MeshletVisibilityWord, VisibleMeshlets);
    }

    // Every visible block stores the same stamp, so an occluded block of the same instance cannot undo it.
    if (bLate && bTwoPhaseWork && VisibleMeshlets != 0u && Pass().OutVisibility.Contains(RetainedSlot))
    {
        Pass().OutVisibility.At(RetainedSlot)[0] = Pass().VisibilityTag;
    }
}
