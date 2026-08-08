#include "Includes/AppendBuffer.slang"
#include "Includes/MeshletCullCore.slang"
#include "Includes/MeshletGeometry.slang"

// The meshlet cull, as the amplification stage of the draw that consumes it. One workgroup is one
// FMeshletBlock: up to MESHLET_TASK_GROUP_SIZE meshlets of ONE instance, so every per-instance load is
// workgroup-uniform and only the meshlet bounds are scattered.
//
// TWO-PHASE OCCLUSION. A pyramid built from last frame's depth cannot be trusted to hide anything this
// frame -- the camera moved, and whatever it disoccluded would simply be missing. So the early phase
// treats a Hi-Z reject as a QUESTION, not an answer: it records the lane in DeferBits and draws nothing.
// Mid-frame the pyramid is rebuilt from the depth the early phase just wrote, and the late phase re-asks
// exactly those lanes against it. A meshlet the camera disoccluded fails early, passes late, and lands in
// the same frame. Nothing is ever a frame behind; the stale pyramid is only ever used to SKIP work, never
// to decide the final result.
//
// The late phase re-dispatches the early phase's grid verbatim -- same buckets, same SubDrawCount, same
// blocks -- and filters on DeferBits. That is the whole reason this needs no compacted defer list, no
// second reservation, and no mid-frame pass to size it: one uint per block IS the compaction, and a
// block still maps to one instance, which the payload requires (see FVisTaskPayload).
//
// RULE: no lane may return early. This shader ends in a group barrier and a uniform DispatchMesh, and a
// lane that left before the barrier makes it non-uniform -- undefined behaviour NVIDIA tolerates and AMD
// hangs on, with no page fault and no failing lane to inspect. Every reject sets bKeep = false.

// The push block this stage shares with the mesh stage it dispatches into.
#define Pass MeshletPass

groupshared FVisTaskPayload gsPayload;
// Groupshared rather than a wave intrinsic: a 32-thread workgroup is a full wave on some devices and a
// partial one on others, and the compaction has to be exact either way.
groupshared uint gsKeepBits;
// Lanes the early phase deferred on Hi-Z. One bit per meshlet in the block, published to DeferBits for
// the late phase to pick up.
groupshared uint gsDeferBits;

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

[shader("amplification")]
[numthreads(MESHLET_TASK_GROUP_SIZE, 1, 1)]
void TaskMain(uint3 GroupID : SV_GroupID, uint ThreadID : SV_GroupIndex, uint DrawIndex : SV_DrawIndex)
{
    // ArgBase is the BUCKET. DrawIndex selects which sub-draw slice of that bucket's block region this
    // workgroup belongs to, which is how a bucket larger than maxTaskWorkGroupCount[0] still dispatches
    // in full instead of being truncated.
    const uint ArgIdx    = Pass().ArgBase;
    const uint BlockSlot = DrawIndex * Pass().MaxTaskGroups + GroupID.x;
    const FCullView View = CullViews()[Pass().CullViewIndex];
    const uint Phase     = Pass().CullPhase;
    const bool bLate     = (Phase == CULL_PHASE_LATE);

    const FRenderBucket Bucket = Pass().Buckets[ArgIdx];
    const uint BlockIdx = Bucket.BlockBase + BlockSlot;

    // Bounded by what BuildMeshletBlocks actually APPENDED, not by what CullInstances reserved. The task
    // grid is sized from BlockCapacity (BuildDrawPrefix), so workgroups always cover the whole reserved
    // region -- but the builder only fills [BlockBase, BlockBase + BlockCursor). Every block above that
    // holds whatever the ring held last frame, and Block.InstanceID out of one is an arbitrary index into
    // Instances(): a wild pointer that faults the moment MeshletHeader is dereferenced, on a buffer that
    // may since have been freed. The cursor can run past capacity on an overflow frame (AppendReserveClamped
    // keeps counting so the readback reports DEMAND), hence the min.
    const uint BlocksWritten = min(Bucket.BlockCursor, Bucket.BlockCapacity);
    const bool bBlockInRange = BlockSlot < BlocksWritten;

    if (ThreadID == 0u)
    {
        gsKeepBits  = 0u;
        gsDeferBits = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Workgroup-uniform, because a block is one instance. bBlockInRange is uniform too (it is built from
    // the sub-draw index and the bucket's own bound), so this costs a uniform branch and keeps the tail
    // workgroups of the last sub-draw from reading past the block arena entirely.
    const FMeshletBlock Block = bBlockInRange ? Pass().BlockList[BlockIdx] : (FMeshletBlock)0;
    const uint InstanceID = Block.InstanceID;
    const uint MeshletIdx = Block.MeshletBase + ThreadID;
    bool       bKeep      = bBlockInRange && (ThreadID < Block.Count);

    FGPUInstance* Inst = Instances() + InstanceID;
    bKeep = bKeep && (Instances() != nullptr) && (Inst.MeshletHeader != nullptr);

    // The late phase runs ONLY what the early phase deferred. Everything else in this block either
    // already rasterized or was rejected by a test whose answer cannot have changed since.
    if (bLate)
    {
        const uint Deferred = bBlockInRange ? Pass().DeferBits[BlockIdx] : 0u;
        bKeep = bKeep && ((Deferred & (1u << ThreadID)) != 0u);
    }

    FMeshletRef Ref = (FMeshletRef)0;
    uint MeshletSlot = 0u;

    if (bKeep)
    {
        const FMeshletCullContext Ctx = MakeMeshletCullContext(Inst);
        FMeshletBoundsCache Cache     = MakeMeshletBoundsCache();

        const FMeshletCullResult Cull = TestMeshletAgainstView(Ctx, Inst, View, MeshletIdx, Cache, Phase);
        bKeep = Cull.bKeep;

        if (Cull.bDeferHiZ)
        {
            InterlockedOr(gsDeferBits, 1u << ThreadID);
        }
    }

    if (bKeep)
    {
        MeshletSlot = EmitMeshletSlot(ArgIdx, InstanceID, MeshletIdx);
        bKeep = MeshletSlot != kAppendFull;
    }

    if (bKeep)
    {
        // Here rather than in the mesh shader: 32 lanes walk 32 dependent pointer chains concurrently
        // instead of one mesh workgroup parking 64 lanes on its own.
        Ref   = ResolveMeshlet(MeshletSlot, View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY));
        bKeep = Ref.M.VertexCount > 0u && Ref.M.TriangleCount > 0u;
    }

    if (bKeep)
    {
        InterlockedOr(gsKeepBits, 1u << ThreadID);
    }

    GroupMemoryBarrierWithGroupSync();

    // Published UNCONDITIONALLY, zero included. Both phases dispatch the same grid over the same blocks,
    // so every word the late phase reads was written by the early phase this frame -- which is what lets
    // DeferBits skip a clear pass entirely. Make this conditional and a block that deferred nothing would
    // hand the late phase last frame's bits for a block that now holds a different instance.
    if (!bLate && ThreadID == 0u && bBlockInRange)
    {
        Pass().DeferBits[BlockIdx] = gsDeferBits;
    }

    const uint KeepBits = gsKeepBits;
    const uint Total    = countbits(KeepBits);

    if (bKeep)
    {
        // (1 << 0) - 1 == 0 and (1 << 31) - 1 == 0x7FFFFFFF, so both ends of the word are exact.
        const uint Out = countbits(KeepBits & ((1u << ThreadID) - 1u));

        gsPayload.Entries[Out].M                = Ref.M;
        gsPayload.Entries[Out].MeshletSlot      = MeshletSlot;
        gsPayload.Entries[Out].GlobalMeshletIdx = Ref.GlobalMeshletIdx;

        gsPayload.InstanceID        = Ref.InstanceID;
        gsPayload.SkinnedVertexBase = Ref.SkinnedVertexBase;
    }

    // Uniform: every lane read the same word after the same barrier. Zero workgroups is legal.
    DispatchMesh(Total, 1, 1, gsPayload);
}
