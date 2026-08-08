#include "Includes/AppendBuffer.slang"
#include "Includes/MeshletCullCore.slang"
#include "Includes/MeshletGeometry.slang"

// The meshlet cull, as the amplification stage of the draw that consumes it.
//
// One workgroup = one FMeshletBlock = up to MESHLET_TASK_GROUP_SIZE meshlets of ONE instance, with that
// instance's range for THIS view already resolved by BuildMeshletBlocks. So every per-instance load --
// FGPUInstance (128 B), FMeshletHeader (80 B), the model matrix -- is workgroup-uniform, and the only
// per-lane scattered read is the meshlet's own bounds.
//
// Two phases, selected by SPEC_TASK_PHASE:
//   0  walk this (view, draw)'s block region, test each meshlet, emit survivors and append the ones the
//      Hi-Z hid to the defer arena.
//   1  walk this phase's end of the defer arena and re-test against the pyramid rebuilt from this frame's
//      own raster. The frustum, cone and micro-poly rejects already fired in phase 0 and are not repeated.
//
// RULE, inherited from MeshletCullCore and load-bearing here specifically: no lane may return early. This
// shader ends in a group barrier and a uniform DispatchMesh, and a lane that left before the barrier makes
// it non-uniform -- undefined behaviour that NVIDIA tolerates and AMD hangs on, with no page fault and no
// failing lane to inspect. Every reject below sets bKeep = false and falls through.

[vk::constant_id(8)] const uint SPEC_TASK_PHASE = 0;

#define TASK_PHASE_BLOCKS 0u
#define TASK_PHASE_DEFER  1u

// FMeshletPassArgs (MeshletGeometry.slang) is the push block this stage shares with the mesh stage it
// dispatches into. CascadeLateViewBase is not needed: a defer entry's cascade is implied by which end of
// the arena it sits at, and this draw already knows its own view.
#define Pass MeshletPass

groupshared FVisTaskPayload gsPayload;
// Survivor bitmask for the workgroup's lanes. A groupshared word rather than a wave intrinsic: a
// 32-thread workgroup is a full wave on some devices and a partial one on others, and the compaction has
// to be exact either way.
groupshared uint gsKeepBits;

// Appends a surviving meshlet into this (view, draw)'s draw-list region and returns its slot, or
// kAppendFull when the region is full. Centralized so both phases share the protocol.
//
// The region is sized to exactly what this pair can emit, so on a frame whose total fits the allocation
// this can never reject. It only fires when the allocation itself came up short, and BuildDrawPrefix
// already collapsed regions that could not fit to zero length.
uint EmitMeshletSlot(uint ArgIdx, uint InstanceID, uint MeshletIdx)
{
    const uint Slot = AppendReserve(Pass().ViewDrawCursors + ArgIdx,
                                    Pass().ViewDrawCounts[ArgIdx],
                                    Pass().ViewDrawOffsets[ArgIdx]);
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

// Queue a meshlet for the late re-test, into THIS (view, draw)'s defer region.
//
// The region is bounded by the draw region's length, and that bound is exact rather than conservative: a
// meshlet either emits or defers, never both, so the space CullInstances reserved for emits is already an
// upper bound on defers. Nothing here can overflow, which is why the defer list carries no feedback,
// no capacity readback and no drop path -- all of which the old global list needed.
void DeferMeshlet(uint ArgIdx, uint InstanceID, uint MeshletIdx)
{
    const uint Slot = AppendReserve(Pass().DeferCursors + ArgIdx,
                                    Pass().ViewDrawCounts[ArgIdx],
                                    Pass().DeferOffsets[ArgIdx]);
    if (Slot == kAppendFull)
    {
        return;
    }

    FMeshletDeferred Entry;
    Entry.InstanceID   = InstanceID;
    Entry.MeshletIndex = MeshletIdx;
    Pass().DeferList[Slot] = Entry;
}

[shader("amplification")]
[numthreads(MESHLET_TASK_GROUP_SIZE, 1, 1)]
void TaskMain(uint3 GroupID : SV_GroupID, uint ThreadID : SV_GroupIndex, uint DrawIndex : SV_DrawIndex)
{
    const uint ArgIdx = Pass().ArgBase + DrawIndex;
    const FCullView View = CullViews()[Pass().CullViewIndex];

    if (ThreadID == 0u)
    {
        gsKeepBits = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Resolved per lane below; workgroup-uniform on the block path because a block is one instance.
    uint InstanceID = 0u;
    uint MeshletIdx = 0u;
    bool bKeep      = false;

    if (SPEC_TASK_PHASE == TASK_PHASE_BLOCKS)
    {
        const FMeshletBlock Block = Pass().BlockList[Pass().ViewBlockOffsets[ArgIdx] + GroupID.x];
        InstanceID = Block.InstanceID;
        MeshletIdx = Block.MeshletBase + ThreadID;
        bKeep      = ThreadID < Block.Count;
    }
    else
    {
        // The region the EARLY view for this draw deferred into. A late view has no blocks of its own --
        // its work is exactly what its early counterpart could not resolve -- so DeferSrcArgBase points
        // at that counterpart's slot rather than at this draw's own.
        const uint SrcIdx = Pass().DeferSrcArgBase + DrawIndex;
        const uint Count  = min(Pass().DeferCursors[SrcIdx], Pass().ViewDrawCounts[SrcIdx]);
        const uint Local  = GroupID.x * MESHLET_TASK_GROUP_SIZE + ThreadID;

        bKeep = Local < Count;

        if (bKeep)
        {
            const FMeshletDeferred Entry = Pass().DeferList[Pass().DeferOffsets[SrcIdx] + Local];
            InstanceID = Entry.InstanceID;
            MeshletIdx = Entry.MeshletIndex;
        }
    }

    FGPUInstance* Inst = Instances() + InstanceID;
    bKeep = bKeep && (Instances() != nullptr) && (Inst.MeshletHeader != nullptr);

    FMeshletRef Ref = (FMeshletRef)0;
    uint MeshletSlot = 0u;

    if (bKeep)
    {
        const FMeshletCullContext Ctx = MakeMeshletCullContext(Inst);
        FMeshletBoundsCache Cache     = MakeMeshletBoundsCache();

        if (SPEC_TASK_PHASE == TASK_PHASE_BLOCKS)
        {
            const FMeshletCullResult R = TestMeshletAgainstView(Ctx, Inst, View, MeshletIdx, Cache);

            // R.bEmit is already false for a deferred meshlet, so a draw that cannot defer drops it here
            // by simply not queueing it -- no separate reject path, and no way for the two to disagree.
            if (R.bDefer && Pass().CanDefer != 0u)
            {
                DeferMeshlet(ArgIdx, InstanceID, R.MeshletIdx);
            }
            bKeep = R.bEmit;
        }
        else
        {
            // Re-test against the pyramid rebuilt from THIS frame's own raster. Still-occluded meshlets
            // are genuinely hidden; ones that now pass were hidden by a stale pyramid, which is the
            // one-frame disocclusion lag the second phase exists to remove.
            RefreshMeshletBounds(Inst, Ctx.Model, Ctx.MaxScale, MeshletIdx, Cache);

            bKeep = View.HasFlag(CULL_VIEW_FLAG_CASCADE)
                  ? !IsCascadeHZBOccluded(View.CascadeIndex, Cache.WorldCenter, Cache.WorldRadius, /*bMid*/ true)
                  : !IsHiZOccluded(Cache.WorldCenter, Cache.WorldRadius);
        }
    }

    if (bKeep)
    {
        MeshletSlot = EmitMeshletSlot(ArgIdx, InstanceID, MeshletIdx);
        bKeep = MeshletSlot != kAppendFull;
    }

    if (bKeep)
    {
        // The slot was just written, so this resolves out of memory this workgroup produced. Doing it
        // here rather than in the mesh shader is the whole point of the amplification stage: 32 lanes
        // walk 32 dependent pointer chains concurrently instead of one mesh workgroup parking 64 lanes
        // on its own.
        Ref   = ResolveMeshlet(MeshletSlot, View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY));
        bKeep = Ref.M.VertexCount > 0u && Ref.M.TriangleCount > 0u;
    }

    if (bKeep)
    {
        InterlockedOr(gsKeepBits, 1u << ThreadID);
    }

    GroupMemoryBarrierWithGroupSync();

    const uint KeepBits = gsKeepBits;
    const uint Total    = countbits(KeepBits);

    if (bKeep)
    {
        // Dense slot: survivors below this lane. (1 << 0) - 1 == 0 and (1 << 31) - 1 == 0x7FFFFFFF, so
        // both ends of the word are exact.
        const uint Out = countbits(KeepBits & ((1u << ThreadID) - 1u));

        gsPayload.Entries[Out].M                = Ref.M;
        gsPayload.Entries[Out].MeshletSlot      = MeshletSlot;
        gsPayload.Entries[Out].GlobalMeshletIdx = Ref.GlobalMeshletIdx;

        // Workgroup-uniform on the block path (one block is one instance) and effectively so on the defer
        // path, where a divergent instance would simply write the same field twice with its own value.
        // Kept out of the per-lane array either way -- see FVisTaskPayload.
        gsPayload.InstanceID        = Ref.InstanceID;
        gsPayload.SkinnedVertexBase = Ref.SkinnedVertexBase;
    }

    // Uniform: every lane read the same word after the same barrier. Launching zero mesh workgroups is
    // legal and is the whole point of the compaction above.
    DispatchMesh(Total, 1, 1, gsPayload);
}
