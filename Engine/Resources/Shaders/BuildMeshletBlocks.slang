#include "Includes/AppendBuffer.slang"
#include "Includes/Culling.slang"

// Builds the cull's work domain: one thread per (visible instance, view), appending that pair's run of
// blocks into the per-(view, draw) region the prefix reserved for it.
//
// Per (view, draw) rather than per instance, because the consumer is a TASK shader. A task workgroup
// belongs to exactly one draw of one view -- it learns which from ArgBase + SV_DrawIndex -- so the blocks
// it walks have to be that view's, with that view's LOD range already resolved. A view-agnostic list
// would force every workgroup to re-derive which view it was culling for.
//
// Resolving the range HERE is what removes the LOD pick from the cull entirely. It also puts the pick in
// the same place, and the same stage, as CullInstances' reservation: the two must agree bit-for-bit or
// the reserved region and the emitted blocks describe different meshlets (see Culling.slang). They are
// now adjacent compute shaders reading the same FSurfaceDesc buffer, rather than one compute shader and
// one raster stage compiled by a different backend.
//
// The alternative -- an inclusive prefix sum over per-instance meshlet counts, a flat grid over the
// summed total, and a binary search per thread to recover which instance and index it owns -- costs three
// scan dispatches, and at 500k instances the search is ~19 DEPENDENT four-byte loads through a pointer
// nothing can prefetch past, paid by every cull thread before it does any work.

struct FBuildMeshletBlocksArgs
{
    uint            NumViews;
    uint            NumBatches;
    // Capacity of the visible-instance buffer. CullInstances' append cursor keeps counting past it on an
    // overflow frame, so the count has to be clamped here exactly as BuildDrawPrefix clamps it -- same two
    // inputs, same min, so the two cannot disagree about which slots are safe to read.
    uint            MaxVisibleInstances;
    uint            NumSurfaceDescs;      // bounds the per-view LOD lookup, as in CullInstances

    uint*           InstanceCount;        // in:  [0] = survivor count, uncapped
    FSurfaceDesc*   SurfaceDescs;
    uint*           ViewBlockOffsets;     // in:  per-(view, draw) region base
    uint*           ViewBlockCounts;      // in:  per-(view, draw) region length
    uint*           BlockCursors;         // in/out: per-(view, draw) append cursor, zeroed each frame
    FMeshletBlock*  OutBlockList;
};
FBuildMeshletBlocksArgs* Pass() { return GetPassArgs<FBuildMeshletBlocksArgs>(); }

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 ThreadID : SV_DispatchThreadID)
{
    const uint NumViews         = max(Pass().NumViews, 1u);
    const uint VisibleInstances = min(Pass().InstanceCount[0], Pass().MaxVisibleInstances);

    // Undo the Y fold the dispatch applies past MAX_DISPATCH_AXIS groups. Reading ThreadID.x alone --
    // which this did -- makes every Y row repeat the SAME range and never reach the tail: past
    // MAX_DISPATCH_AXIS * 64 threads that is a double append of the low instances and no blocks at all
    // for the high ones. Same fold CullInstances and Skinning already undo.
    const uint Flat       = ThreadID.x + ThreadID.y * MAX_DISPATCH_AXIS * 64u;
    const uint InstanceID = Flat / NumViews;
    const uint ViewIndex  = Flat % NumViews;

    if (InstanceID >= VisibleInstances || ViewIndex >= Pass().NumViews)
    {
        return;
    }

    FGPUInstance* Inst = Instances() + InstanceID;
    if (Inst.MeshletHeader == nullptr)
    {
        return;
    }

    const FCullView View = CullViews()[ViewIndex];

    // The late views own the defer list, not the block list: their work is whatever phase 0 deferred, and
    // they never walk the scene.
    if (View.HasFlag(CULL_VIEW_FLAG_PHASE_LATE))
    {
        return;
    }

    // Instance-level reject that needs no meshlet. A block built for a view that will reject every one of
    // its meshlets is pure launch overhead.
    if (View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY) && !Inst.HasFlag(EInstanceFlags::CastShadow))
    {
        return;
    }

    // Which range this view rasters. Byte-for-byte the pick CullInstances reserved this region from.
    const bool bPerViewLOD = Inst.SurfaceDescIndex != kNoSurfaceDesc
                          && Inst.SurfaceDescIndex < Pass().NumSurfaceDescs;

    FSurfaceDesc Desc = (FSurfaceDesc)0;
    if (bPerViewLOD)
    {
        Desc = Pass().SurfaceDescs[Inst.SurfaceDescIndex];
    }

    const bool bShadowView = View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY);

    uint2 Range;
    if (bShadowView && bPerViewLOD && View.HasFlag(CULL_VIEW_FLAG_CASCADE))
    {
        Range = SelectCascadeShadowMeshletRange(Desc, GetCameraPosition(), Inst.SphereBounds,
                                                View.CascadeIndex,
                                                Scene().CullData.ShadowLODBias,
                                                Scene().CullData.ShadowCoarseLODDistSq);
    }
    else if (bShadowView)
    {
        Range = uint2(Inst.ShadowMeshletOffset, Inst.ShadowMeshletCount);
    }
    else if (bPerViewLOD)
    {
        Range = SelectViewMeshletRange(Desc, View.ViewOrigin, Inst.SphereBounds);
    }
    else
    {
        Range = uint2(Inst.SurfaceMeshletOffset, Inst.SurfaceMeshletCount);
    }

    // Mesh-global bound. Range.y says how long the range is, not that Range.x + it lands inside the
    // header's arrays; an unbounded base walks Bounds[] past the allocation, and those are scattered BDA
    // reads that return garbage rather than faulting -- a multi-second hang and a TDR with no page fault.
    const uint MeshletTotal = Inst.MeshletTotalCount;
    if (Range.y == 0u || Range.x >= MeshletTotal)
    {
        return;
    }
    const uint Count = min(Range.y, MeshletTotal - Range.x);

    const uint ArgIdx = ViewIndex * Pass().NumBatches + Inst.DrawID;
    const uint Blocks = (Count + (MESHLET_TASK_GROUP_SIZE - 1u)) / MESHLET_TASK_GROUP_SIZE;

    // One atomic for the instance's whole run: its blocks are contiguous by construction. Clamped rather
    // than all-or-nothing, so a region that came up short still culls the meshlets it had room for --
    // and the region is sized to exactly what this (view, draw) can produce, so that only happens when
    // the whole allocation did. BuildDrawPrefix already collapsed regions that could not fit to zero
    // length, which makes Granted 0 for those.
    uint Granted;
    const uint Slot = AppendReserveClamped(Pass().BlockCursors + ArgIdx,
                                           Pass().ViewBlockCounts[ArgIdx],
                                           Pass().ViewBlockOffsets[ArgIdx],
                                           Blocks, Granted);

    for (uint b = 0u; b < Granted; ++b)
    {
        const uint First = b * MESHLET_TASK_GROUP_SIZE;

        FMeshletBlock Block;
        Block.InstanceID         = InstanceID;
        Block.PackedBaseAndCount = FMeshletBlock::Pack(Range.x + First,
                                                       min(MESHLET_TASK_GROUP_SIZE, Count - First));
        Pass().OutBlockList[Slot + b] = Block;
    }
}
