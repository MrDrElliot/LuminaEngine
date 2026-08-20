#include "Includes/AppendBuffer.slang"
#include "Includes/MeshletCullCore.slang"
#include "Includes/MeshletGeometry.slang"

// Per-meshlet culling, and the sole authority on what rasterizes. One workgroup is one FMeshletBlock:
// up to MESHLET_CULL_GROUP_SIZE meshlets of ONE instance, so every per-instance load is workgroup-uniform
// and only the meshlet bounds are scattered. Survivors are appended to MeshletDrawList, which every
// geometry pass then draws straight out of.
//
// Classic two-phase occlusion. PrevVisibility[RetainedSlot] holds "this instance was visible to the
// camera at the end of last frame":
//
//   EARLY  replays that set with NO Hi-Z test, producing the depth the pyramid is built from.
//   LATE   tests EVERY instance against that fresh pyramid, emits only the ones early did not draw,
//          and accumulates next frame's set into OutVisibility.
//
// The two draw sets partition the frustum-visible set, so whatever the flag holds -- stale, raced, or
// wrong -- each instance rasterizes exactly once. The flag only decides which phase, i.e. how good the
// occluder set is. Both dispatches must read the SAME flags, hence the separate write target below.
//
// Only views flagged CULL_VIEW_FLAG_MESHLET_HIZ are two-phase. Every other view (shadow cascades) is
// culled in full by the early dispatch and skipped entirely by the late one, which is what stops it
// emitting its meshlets twice.
//
// RULE inherited from the task stage: no lane may return early. The group barrier below must be reached
// by every lane, and a lane that left before it makes the barrier non-uniform -- undefined behaviour
// NVIDIA tolerates and AMD hangs on. Every reject sets bKeep = false.

struct FMeshletCullArgs
{
    uint            NumViews;
    // A bucket index is ViewIndex * NumDraws + DrawID, which is how a workgroup recovers its cull view.
    uint            NumDraws;
    // MESHLET_SLICE_EARLY or MESHLET_SLICE_LATE.
    uint            Slice;
    // Bounds RetainedSlot, and 0 when either buffer is missing, so the reads short-circuit.
    uint            VisibilityCapacity;

    FRenderBucket*  Buckets;
    FMeshletBlock*  BlockList;
    // Read by BOTH dispatches and written by neither: the partition needs them to see one set of flags.
    uint*           PrevVisibility;
    // OR only: an instance's blocks span workgroups, so a store lets an occluded one clear a visible bit.
    uint*           OutVisibility;
};
FMeshletCullArgs* Pass() { return GetPassArgs<FMeshletCullArgs>(); }

// Set by any lane that keeps a meshlet, read by lane 0 after the barrier. Groupshared rather than a wave
// intrinsic: a 32-thread workgroup is a full wave on some devices and a partial one on others.
groupshared uint gsAnyVisible;

uint EmitMeshletSlot(uint ArgIdx, uint InstanceID, uint MeshletIdx)
{
    FRenderBucket* Bucket = Pass().Buckets + ArgIdx;
    const uint Slot = AppendReserve(&Bucket.DrawCursor, Bucket.DrawCapacity, Bucket.DrawBase);
    if (Slot == kAppendFull)
    {
        return kAppendFull;
    }

    FMeshletDraw Draw;
    Draw.InstanceID        = InstanceID;
    Draw.PackedIndexAndTag = FMeshletDraw::Pack(MeshletIdx, Scene().CullData.MeshletDrawTag);
    MeshletDrawList()[Slot] = Draw;

    return Slot;
}

// Flat workgroup -> owning bucket. Blocks are dense inside a bucket but sparse across the arena, so the
// bucket cannot be read from the block; BuildMeshletCullArgs writes a dense prefix and this inverts it.
// Workgroup-uniform, and ~9 steps for the hundreds of buckets a frame has.
uint FindBucketForWork(uint WorkIndex, uint NumBuckets)
{
    uint Lo = 0u;
    uint Hi = NumBuckets;
    while (Lo + 1u < Hi)
    {
        const uint Mid = (Lo + Hi) >> 1u;
        if (Pass().Buckets[Mid].CullWorkBase <= WorkIndex)
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

    // The grid is folded up to a rectangle, so the tail workgroups have no block. They still have to
    // reach the barrier below, so this gates the work rather than returning.
    uint ArgIdx        = 0u;
    uint BlockIdx      = 0u;
    bool bBlockInRange = false;

    if (NumBuckets > 0u)
    {
        ArgIdx = FindBucketForWork(WorkIndex, NumBuckets);

        const FRenderBucket Bucket = Pass().Buckets[ArgIdx];
        const uint BlockSlot = WorkIndex - Bucket.CullWorkBase;

        // Bounded by what BuildMeshletBlocks APPENDED, not what CullInstances reserved: blocks above the
        // cursor hold last frame's ring, and an InstanceID out of one is a wild pointer.
        const uint BlocksWritten = min(Bucket.BlockCursor, Bucket.BlockCapacity);
        bBlockInRange = BlockSlot < BlocksWritten;
        BlockIdx      = Bucket.BlockBase + BlockSlot;
    }

    // Workgroup-uniform: ArgIdx is, and the view is a pure function of it.
    const FCullView View  = CullViews()[ArgIdx / NumDraws];
    const bool bTwoPhase  = View.HasFlag(CULL_VIEW_FLAG_MESHLET_HIZ);

    // A single-phase view belongs entirely to the early dispatch. Skipping it in the late one is what
    // keeps a cascade's meshlets out of the draw list twice -- both dispatches share the bucket.
    const bool bHasWork = bBlockInRange && (!bLate || bTwoPhase);

    if (ThreadID == 0u)
    {
        gsAnyVisible = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Workgroup-uniform, so this is a uniform branch: blocks the builder never filled are never read.
    const FMeshletBlock Block = bHasWork ? Pass().BlockList[BlockIdx] : (FMeshletBlock)0;
    const uint InstanceID = Block.InstanceID;
    const uint MeshletIdx = Block.MeshletBase + ThreadID;

    // Guarded rather than folded into an && chain: this is the deepest dependent hop any lane makes
    // before the cull proper, and unconditional it was paid by workgroups with nothing to do.
    //
    // The header used to be null-checked here. It cannot be null now -- an instance carries a slot and
    // slot 0 is the null header -- so what is left is the instance buffer itself, which the CPU can
    // legitimately fail to allocate. TestMeshletAgainstView bounds every read by the header's own count.
    FGPUInstance* Inst = Instances() + InstanceID;
    const bool bInstanceOK = bHasWork && (Instances() != nullptr);

    // Workgroup-uniform once the block is loaded, so both the load and the branches below are uniform.
    uint RetainedSlot = 0u;
    bool bWasVisible  = false;
    if (bInstanceOK && bTwoPhase)
    {
        RetainedSlot = Inst.RetainedSlot;
        bWasVisible  = (RetainedSlot < Pass().VisibilityCapacity)
                     && (Pass().PrevVisibility[RetainedSlot] != 0u);
    }

    // EARLY emits exactly last frame's visible set; LATE emits exactly its complement. Disjoint, so no
    // instance rasterizes twice and none is missed by both. A single-phase view emits from EARLY always.
    const bool bEmitPhase = !bTwoPhase || (bLate != bWasVisible);

    // LATE tests everything, including instances EARLY already drew and it will not emit: that test is
    // what rewrites the flag for next frame. EARLY only tests what it can emit -- an instance it is not
    // going to draw costs it nothing.
    const bool bTest = bInstanceOK && (ThreadID < Block.Count) && (bLate || bEmitPhase);
    bool       bKeep = bTest && bEmitPhase;

    if (bTest)
    {
        const FMeshletCullContext Ctx = MakeMeshletCullContext(Inst);
        FMeshletBoundsCache Cache     = MakeMeshletBoundsCache();

        // The early phase is a replay and must not Hi-Z cull: a meshlet dropped there is dropped for
        // good, because the late phase does not emit for instances the early phase drew.
        const bool bVisible = TestMeshletAgainstView(Ctx, Inst, View, MeshletIdx, Cache, /*bApplyHiZ*/ bLate);

        bKeep = bKeep && bVisible;

        if (bVisible && bLate && bTwoPhase)
        {
            InterlockedOr(gsAnyVisible, 1u);
        }
    }

    if (bKeep)
    {
        // The append is the whole output: a slot in MeshletDrawList IS the survivor, and the mesh stage
        // resolves it back into a meshlet. Refused only when the bucket's region is full.
        bKeep = EmitMeshletSlot(ArgIdx, InstanceID, MeshletIdx) != kAppendFull;
    }

    GroupMemoryBarrierWithGroupSync();

    // OR only, never a store: an instance's blocks are spread across workgroups, and a plain write would
    // let a fully-occluded block clear a flag a visible one had just set. The zero comes from the clear
    // the CPU issues against this buffer at the top of the frame.
    if (bLate && bTwoPhase && ThreadID == 0u && gsAnyVisible != 0u && RetainedSlot < Pass().VisibilityCapacity)
    {
        InterlockedOr(Pass().OutVisibility[RetainedSlot], 1u);
    }
}
