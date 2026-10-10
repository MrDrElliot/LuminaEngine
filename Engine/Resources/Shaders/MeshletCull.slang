#include "Includes/AppendBuffer.slang"
#include "Includes/MeshletCullCore.slang"
#include "Includes/MeshletGeometry.slang"

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
    uint            _Pad0;

    GPUSpan<FRenderBucket> Buckets;
    GPUSpan<FMeshletBlock> BlockList;
    // Read by both dispatches and written by neither, so the partition sees one set of flags. A missing buffer has count 0.
    GPUSpan<uint>          PrevVisibility;
    // Stamped with VisibilityTag by every visible block; identical stores need no atomic and no clear.
    GPUSpan<uint>          OutVisibility;
    // One word per block of an instance's camera LOD. Both phases read it, and only the late phase replaces it.
    GPUSpan<uint>          MeshletVisibility;
};
FMeshletCullArgs* Pass() { return GetPassArgs<FMeshletCullArgs>(); }

#if MESHLET_CULL_GROUP_SIZE > 32
#error "MESHLET_CULL_GROUP_SIZE must fit one wave, or WaveActiveAnyTrue below covers only part of the group."
#endif

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

[shader("compute")]
[numthreads(MESHLET_CULL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint ThreadID : SV_GroupIndex)
{
    const uint NumDraws   = max(Pass().NumDraws, 1u);
    const uint NumBuckets = Pass().NumViews * NumDraws;
    const bool bLate      = (Pass().Slice == MESHLET_SLICE_LATE);

    // Undoes the Y fold BuildMeshletCullArgs applies past MAX_DISPATCH_AXIS groups.
    const uint WorkIndex = GroupID.x + GroupID.y * (uint)MAX_DISPATCH_AXIS;

    // Tail workgroups of the folded grid have no block, so this gates the work rather than returning.
    uint ArgIdx        = 0u;
    uint BlockIdx      = 0u;
    bool bBlockInRange = false;

    if (NumBuckets > 0u)
    {
        ArgIdx = FindBucketForWork(WorkIndex, NumBuckets);

        const FRenderBucket Bucket = Pass().Buckets.GetAligned(ArgIdx);
        const uint BlockSlot = WorkIndex - Bucket.CullWorkBase;

        // The late phase of a split bucket culls only the blocks the late build appended at the region's end.
        const bool bBackRegion = bLate && Pass().bSplitPhases != 0u && CullViewHasFlag(ArgIdx / NumDraws, CULL_VIEW_FLAG_MESHLET_HIZ)
                              && BucketFitsDemand(Bucket.DrawDemand, Bucket.DrawCapacity, Bucket.BlockDemand, Bucket.BlockCapacity);

        // Bounded by what was appended, not what CullInstances reserved, since unwritten blocks hold last frame's ring.
        const uint Front         = min(Bucket.BlockCursor, Bucket.BlockCapacity);
        const uint BlocksWritten = bBackRegion ? min(Bucket.BlockBackCount, Bucket.BlockCapacity - Front) : Front;
        bBlockInRange = BlockSlot < BlocksWritten;
        BlockIdx      = bBackRegion ? (Bucket.BlockBase + Bucket.BlockCapacity - BlocksWritten + BlockSlot)
                                    : (Bucket.BlockBase + BlockSlot);
    }

    // Workgroup-uniform since the view is a pure function of ArgIdx.
    const FCullView View  = CullViews().GetAligned((ArgIdx / NumDraws));
    const bool bTwoPhase  = View.HasFlag(CULL_VIEW_FLAG_MESHLET_HIZ);

    // Single-phase views belong to the early dispatch, and both dispatches share the bucket, so the late one skips them.
    const bool bHasWork = bBlockInRange && (!bLate || bTwoPhase);

    // Workgroup-uniform branch, so blocks the builder never filled are never read.
    const FMeshletBlock Block = bHasWork ? Pass().BlockList.Get(BlockIdx) : (FMeshletBlock)0;
    const uint InstanceID = Block.InstanceID;
    const uint MeshletIdx = Block.MeshletBase + ThreadID;

    // TestMeshletAgainstView bounds every read by the header's own count.
    FGPUInstance* Inst = Instances() + InstanceID;

    // Workgroup-uniform once the block is loaded, so both the load and the branches below are uniform.
    uint RetainedSlot = 0u;
    bool bWasVisible  = false;
    if (bHasWork && bTwoPhase)
    {
        RetainedSlot = Inst.RetainedSlot;
        bWasVisible  = Pass().PrevVisibility.Get(RetainedSlot) == (Pass().VisibilityTag - 1u);
    }

    // A block from the coarser-LOD overflow fallback is not one of the camera range's blocks, so it has no word.
    uint MeshletVisibilityWord = 0u;
    bool bHasMeshletVisibility = false;
    uint MeshletVisibilityBits = ~0u;
    if (bHasWork && bTwoPhase && Inst.MeshletVisibilityBase != kNoMeshletVisibility && Block.MeshletBase >= Inst.SurfaceMeshletOffset)
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
    const bool bWasMeshletVisible = bWasVisible && ((MeshletVisibilityBits >> ThreadID) & 1u) != 0u;

    // Late only, because the early phase is a replay and an instance dropped there is dropped for good.
    bool bInstanceOccluded = false;

    if (bHasWork && bLate && bTwoPhase && !FGPUInstance::HasFlag(Inst, EInstanceFlags::NoCull))
    {
        // The one occlusion test a skinned instance can take, since its sphere is re-fed every frame.
        const float4 Sphere = loadAligned<16>(&Inst.SphereBounds);
        const FCullSphereProjection Proj = ProjectCullSphere(Sphere.xyz, Sphere.w);

        // Uniform across the group, and it leaves every lane invisible so the flag clears for next frame.
        bInstanceOccluded = Proj.bValid && IsHiZOccluded(Proj);
    }

    // Early emits last frame's visible meshlets and late their complement, so each rasterizes once whatever the bits hold.
    const bool bEmitPhase = !bTwoPhase || (bLate != bWasMeshletVisible);

    // Late also tests what early drew, since that test is what writes next frame's flag and bits.
    const bool bTest = bHasWork && !bInstanceOccluded && (ThreadID < Block.Count) && (bLate || bEmitPhase);
    bool       bKeep = bTest && bEmitPhase;
    bool       bLaneVisible = false;

    if (bTest)
    {
        const FMeshletCullContext Ctx = MakeMeshletCullContext(Inst);
        FMeshletBoundsCache Cache     = MakeMeshletBoundsCache();

        // No Hi-Z in the early replay, since the late phase never re-emits what the early phase drew.
        const bool bVisible = TestMeshletAgainstView(Ctx, View, MeshletIdx, Cache, bLate);

        bKeep = bKeep && bVisible;
        bLaneVisible = bVisible;
    }

    // The bucket is workgroup-uniform, so one atomic per wave reserves every survivor's slot.
    const uint WaveKept = WaveActiveCountBits(bKeep);
    if (WaveKept > 0u)
    {
        FRenderBucket* Bucket = Pass().Buckets.At(ArgIdx);
        uint WaveOld = 0u;
        if (WaveIsFirstLane())
        {
            uint* Cursor = &Bucket.DrawCursor;
            InterlockedAdd(Cursor[0], WaveKept, WaveOld);
        }
        WaveOld = WaveReadLaneFirst(WaveOld);

        // The cursor still advances by the full count past capacity, so the readback keeps reporting demand.
        const uint Local = WaveOld + WavePrefixCountBits(bKeep);
        if (bKeep && Local < Bucket.DrawCapacity)
        {
            FMeshletDraw Draw;
            Draw.InstanceID        = InstanceID;
            Draw.PackedIndexAndTag = FMeshletDraw::Pack(MeshletIdx, Scene().CullData.MeshletDrawTag);
            MeshletDrawList().Set(Bucket.DrawBase + Local, Draw);
        }
    }

    // Reached by every lane, so the vote covers the whole group.
    const bool bBlockVisible = WaveActiveAnyTrue(bLaneVisible);
    const uint VisibleMeshlets = WaveActiveBitOr(bLaneVisible ? (1u << ThreadID) : 0u);

    // This block owns its whole word, so a plain store replaces it.
    if (bLate && bHasMeshletVisibility && !bInstanceOccluded && ThreadID == 0u)
    {
        Pass().MeshletVisibility.Set(MeshletVisibilityWord, VisibleMeshlets);
    }

    // Every visible block stores the same stamp, so an occluded block of the same instance cannot undo it.
    if (bLate && bTwoPhase && ThreadID == 0u && bBlockVisible && Pass().OutVisibility.Contains(RetainedSlot))
    {
        Pass().OutVisibility.At(RetainedSlot)[0] = Pass().VisibilityTag;
    }
}
