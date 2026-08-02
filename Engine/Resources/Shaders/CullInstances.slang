#include "Includes/Culling.slang"

// Instance-level cull + LOD selection: the GPU half of the retained scene.
//
// The scene keeps a RETAINED instance array -- one stable slot per primitive surface, written only when
// that primitive actually changes -- and this pass turns it into the per-frame compacted instance buffer
// that every downstream pass already consumes. It replaces the CPU gather outright:
//
//   * visibility   -- a slot survives if it intersects ANY cull view's frustum. That is strictly more
//                     accurate than the CPU pre-cull it replaces, which had to union the camera frustum,
//                     a sun sweep and a sphere per shadow light because it could not afford per-view work.
//   * LOD          -- picked here from the interned FSurfaceDesc, per instance (not per view), matching
//                     the CPU's main-camera distance rule. Shadow LOD is the same pick, biased.
//   * compaction   -- survivors are atomically appended, so the output is dense and FGPUInstance keeps
//                     its exact existing layout. CullMeshlets, the VisBuffer passes, shadows and skinning
//                     are untouched.
//
// It also accumulates the per-batch meshlet totals BuildDrawPrefix turns into the draw-list layout
// (ViewDrawOffsets), which is what let the CPU stop owning the draw-argument layout.

#define LOCAL_SIZE_X 64

struct FCullInstancesArgs
{
    uint  NumRetained;        // slots in the retained array (dense; free slots carry bActive = 0)
    uint  NumViews;
    uint  NumBatches;
    int   ShadowLODBias;

    uint  bUseLODs;
    uint  MaxVisibleInstances;  // capacity of OutInstances; overflow is dropped, not written past
    uint  _Pad0;
    uint  _Pad1;

    FGPUInstance*      RetainedInstances;   // in:  retained, CPU dirty-range written
    FInstanceCullData* RetainedCullData;    // in:  parallel to the above
    FSurfaceDesc*      SurfaceDescs;        // in:  interned per distinct surface

    FGPUInstance*      OutInstances;        // out: compacted survivors (what SceneRoot.Instances points at)
    uint*              OutInstanceCount;    // out: [0] = survivor count, pre-zeroed
    // out: NumBatches entries. VIEW-INDEPENDENT meshlet work: sum of SurfaceMeshletCount over survivors.
    // This is the meshlet cull's dispatch domain, which must cover every surviving instance regardless of
    // which view sees it, so it cannot be derived from the per-view counts below.
    uint*              OutBatchMeshletCount;
    // out: NumViews * NumBatches entries. Exact upper bound on what each (view, draw) can emit, which
    // BuildDrawPrefix scans into the draw list's layout. This is what replaced slicing the draw list into
    // equal per-view chunks: a cascade that culls almost everything now costs almost no space, and the
    // camera gets what it actually needs.
    uint*              OutViewDrawCount;
    uint*              OutOverflowFlag;     // out: [0] set if MaxVisibleInstances was hit
};
FCullInstancesArgs* Pass() { return GetPassArgs<FCullInstancesArgs>(); }

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(uint3 GlobalID : SV_DispatchThreadID, uint3 GroupID : SV_GroupID)
{
    // Same 65535-group Y fold the meshlet cull uses, so a huge retained set still dispatches.
    const uint SlotIdx = GlobalID.x + GroupID.y * 65535u * LOCAL_SIZE_X;
    if (SlotIdx >= Pass().NumRetained)
    {
        return;
    }

    FInstanceCullData Cull = Pass().RetainedCullData[SlotIdx];
    if (Cull.bActive == 0u)
    {
        return;   // freed slot; the retained instance beside it is stale by design
    }

    FGPUInstance Inst = Pass().RetainedInstances[SlotIdx];

    // Skinned instances are CPU-fed at the head of the visible buffer every frame; the retained copy
    // has no valid bone offset or pre-skin base, so appending it would draw a garbage duplicate.
    // The CPU keeps these slots bActive = 0 -- this is the backstop for a flag arriving another way.
    if (Inst.HasFlag(EInstanceFlags::Skinned))
    {
        return;
    }

    const float3 Center = Inst.SphereBounds.xyz;
    const float  Radius = Inst.SphereBounds.w;
    const bool   bNoCull = Inst.HasFlag(EInstanceFlags::NoCull);

    // Distance cut first: it is the cheapest reject and the only one that is view-independent.
    const float3 CamPos = GetCameraPosition();
    const float3 ToCam  = Center - CamPos;
    const float  DistSq = dot(ToCam, ToCam);

    if (!bNoCull && Cull.MaxDrawDistance > 0.0)
    {
        const float CutOff = Cull.MaxDrawDistance + Radius;
        if (DistSq > CutOff * CutOff)
        {
            return;
        }
    }

    // LOD. Per instance and camera-derived, matching the CPU rule this replaces; every view reuses the
    // pick, with shadow views taking the biased one.
    //
    // Resolved BEFORE the per-view visibility loop, because that loop now also accumulates how many
    // meshlets each view could emit for this instance -- which needs the LOD's meshlet counts.
    FSurfaceDesc Desc = Pass().SurfaceDescs[Cull.SurfaceDescIndex];

    uint LODIndex = 0u;
    if (Cull.ForcedLODIndex >= 0)
    {
        LODIndex = min((uint)Cull.ForcedLODIndex, Desc.NumLODs > 0u ? Desc.NumLODs - 1u : 0u);
    }
    else if (Pass().bUseLODs != 0u)
    {
        LODIndex = Desc.SelectLOD(DistSq, Radius * Radius);
    }

    const int  MaxShadowLOD  = (int)min(Desc.NumLODs > 0u ? Desc.NumLODs - 1u : 0u, (uint)MESHLET_MAX_SHADOW_LOD);
    const uint ShadowLODIndex = (uint)clamp((int)LODIndex + Pass().ShadowLODBias, 0, MaxShadowLOD);

    // A null header with a non-zero count would page-fault the meshlet cull, so gate the count on it
    // exactly as the CPU gather did.
    const bool bHasGeometry = Inst.MeshletHeader != nullptr;

    Inst.SurfaceMeshletOffset = Desc.LODMeshletOffset[LODIndex];
    Inst.SurfaceMeshletCount  = bHasGeometry ? Desc.LODMeshletCount[LODIndex] : 0u;
    Inst.ShadowMeshletOffset  = Desc.LODMeshletOffset[ShadowLODIndex];
    Inst.ShadowMeshletCount   = bHasGeometry ? Desc.LODMeshletCount[ShadowLODIndex] : 0u;

    if (Inst.SurfaceMeshletCount == 0u && Inst.ShadowMeshletCount == 0u)
    {
        return;   // nothing to raster; keep it out of the prefix domain entirely
    }

    // Per-view visibility AND per-(view, draw) space reservation in one pass.
    //
    // A shadow-only view still needs the instance uploaded, and a capture camera sees its own frustum, so
    // testing each view directly covers both without the CPU's conservative union. There is no early break
    // any more: every view this instance is visible to has to reserve room for it, so the loop runs to the
    // end. That is strictly more work than the old "visible anywhere?" test, and it is what buys an exactly
    // sized draw list instead of NumViews copies of the worst case.
    const bool bCaster  = Inst.HasFlag(EInstanceFlags::CastShadow);
    const bool bInRange = Inst.DrawID < Pass().NumBatches;
    bool bAnyView = false;

    for (uint v = 0; v < Pass().NumViews; ++v)
    {
        FCullView View = CullViews()[v];

        if (!bNoCull)
        {
            if (View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY) && !bCaster)
            {
                continue;
            }
            if (View.HasFlag(CULL_VIEW_FLAG_FRUSTUM) &&
                !FrustumContainsSphere(View.FrustumPlanes, Center, Radius))
            {
                continue;
            }
            // Cascades narrow further, to their receiver slice swept toward the sun: the ortho box in
            // FrustumPlanes bounds a camera slice's SPHERE, which for the far cascades encloses every
            // nearer one. Mirrors the test in CullMeshlets, at instance granularity -- which is the
            // conservative direction (a meshlet sphere sits inside its instance sphere), so the per-view
            // reservation below still covers everything the meshlet cull can emit.
            if (View.IsCascade() &&
                !FrustumContainsSphere(Scene().CullData.CascadeFrustum[View.CascadeIndex], Center, Radius))
            {
                continue;
            }
        }
        bAnyView = true;

        // What this view can emit for this instance. The meshlet cull walks local indices over the SURFACE
        // count and a shadow-only view additionally skips indices past its own shadow count, so the bound
        // there is the smaller of the two. Every other view (camera early, camera late, captures) is bounded
        // by the surface count. Over-estimating only wastes space; under-estimating drops meshlets.
        // A shadow view emits over its own LOD's meshlet range, which is capped at MESHLET_MAX_SHADOW_LOD
        // and so can be FINER -- and therefore LARGER -- than the camera LOD's. This used to reserve
        // min(surface, shadow), which matched a cull that could only ever walk the surface count; now that
        // the walk covers both, reserving the min would drop exactly the meshlets the widened walk found.
        const bool bShadowView = View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY);
        const uint Emitted     = bShadowView ? Inst.ShadowMeshletCount : Inst.SurfaceMeshletCount;

        if (Emitted > 0u && bInRange)
        {
            InterlockedAdd(Pass().OutViewDrawCount[v * Pass().NumBatches + Inst.DrawID], Emitted);
        }
    }

    if (!bAnyView)
    {
        return;
    }

    // Append. The output order is arbitrary and that is fine: the meshlet draw list carries InstanceID
    // explicitly, and the per-instance prefix is built from whatever order lands here.
    uint OutSlot;
    InterlockedAdd(Pass().OutInstanceCount[0], 1u, OutSlot);

    if (OutSlot >= Pass().MaxVisibleInstances)
    {
        // Feedback sizing: record it and drop. The CPU grows the buffer from the count it reads back,
        // so an overflow costs one frame of missing instances rather than a write past the allocation.
        Pass().OutOverflowFlag[0] = 1u;
        return;
    }

    // View-independent meshlet work for the meshlet cull's dispatch domain. Separate from the per-view
    // counts above because an instance visible to three views contributes to three of those but is walked
    // exactly once by the cull.
    //
    // Bounds-checked against the batch count the counter array was sized from. A retained instance is
    // stamped with its DrawID on the game thread and the array is sized from the same registry, so a
    // mismatch means the two drifted -- and an unchecked atomic there would silently corrupt whatever
    // allocation follows, which then shows up as some unrelated shader faulting.
    if (bInRange)
    {
        // Same max as the prefix scan: this is the cull's dispatch domain and the prefix is what indexes
        // into it, so the two have to be the same number or the tail of the domain resolves nothing.
        InterlockedAdd(Pass().OutBatchMeshletCount[Inst.DrawID],
                       max(Inst.SurfaceMeshletCount, Inst.ShadowMeshletCount));
    }

    Pass().OutInstances[OutSlot] = Inst;
}
