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
//   * LOD          -- picked here from the interned FSurfaceDesc, PER VIEW, from each view's own origin
//                     (SelectSurfaceLODForView). The pick decides how much draw-list space that view
//                     reserves for the instance; CullMeshlets makes the identical pick when it walks.
//                     Shadow views take the camera-distance pick, biased -- see FGPUInstance.
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
    // ShadowLODBias and ShadowCoarseLODDistSq used to sit here. They moved to Scene().CullData because the
    // meshlet cull now re-derives the cascade shadow pick too, and two copies of a pick's inputs is two
    // things to drift apart.
    uint  bUseLODs;

    uint  MaxVisibleInstances;  // capacity of OutInstances; overflow is dropped, not written past
    // Entries of SurfaceDescs the CPU has actually uploaded. A slot's SurfaceDescIndex is stamped when it
    // binds; the table is a separate allocation with its own upload, so the bound belongs in the shader
    // rather than in the invariant that the two agree.
    uint  NumSurfaceDescs;

    // The retained scene, split by ACCESS RATE. CullEntries is streamed in full by every lane below;
    // the other two are read ONLY once an instance has survived, which is what the split is for.
    FInstanceCullEntry* RetainedCullEntries; // in:  32 B/slot, CPU dirty-range written
    FTransform3x4*      RetainedTransforms;  // in:  parallel; 48 B affine, rewritten whenever a primitive moves
    FInstanceStatic*    RetainedStatic;      // in:  parallel; rewritten only on a re-bind
    FSurfaceDesc*       SurfaceDescs;        // in:  interned per distinct surface

    FGPUInstance*      OutInstances;        // out: compacted survivors (what SceneRoot.Instances points at)
    uint*              OutInstanceCount;    // out: [0] = survivor count, pre-zeroed
    // out: NumBatches entries. VIEW-INDEPENDENT meshlet work: sum of MeshletWalkCount over survivors.
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

    // THE only load on the reject path: 32 bytes, two slots per cache line. Everything from here to the
    // append comes out of this one entry -- the transform and the static payload are not touched until
    // the instance has earned them.
    FInstanceCullEntry Cull = Pass().RetainedCullEntries[SlotIdx];

    if (!Cull.HasFlag(EInstanceFlags::Active))
    {
        return;   // freed slot; the payload beside it is stale by design
    }

    // Skinned instances are CPU-fed at the head of the visible buffer every frame; the retained copy
    // has no valid bone offset or pre-skin base, so appending it would draw a garbage duplicate.
    // The CPU keeps these slots inactive -- this is the backstop for a flag arriving another way.
    if (Cull.HasFlag(EInstanceFlags::Skinned))
    {
        return;
    }

    const float3 Center = Cull.SphereBounds.xyz;
    const float  Radius = Cull.SphereBounds.w;
    const bool   bNoCull = Cull.HasFlag(EInstanceFlags::NoCull);

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

    // Default LOD: camera-derived, matching the CPU rule this replaces. Every view that cannot re-select
    // (forced LOD, LODs off) uses it as-is, the shadow pick is biased off it, and the per-view picks in the
    // loop below start from the same desc.
    //
    // Resolved BEFORE the per-view visibility loop, because that loop also accumulates how many meshlets
    // each view could emit for this instance -- which needs the LOD's meshlet counts.

    // The index is bounded, because the consequence of trusting it is not a wrong LOD. Past the table this
    // reads whatever follows the allocation, and the meshlet counts below are accumulated into
    // BatchMeshletCounts, which BuildDrawPrefix sums into the meshlet cull's INDIRECT DISPATCH SIZE. An
    // out-of-range index here is therefore a grid size, not a visual artifact.
    if (Cull.SurfaceDescIndex >= Pass().NumSurfaceDescs)
    {
        return;
    }

    FSurfaceDesc Desc = Pass().SurfaceDescs[Cull.SurfaceDescIndex];

    // The LOD arrays are MESHLET_MAX_LODS long; NumLODs comes through the desc and indexes them. Clamping
    // it here means a bad table cannot index the arrays out of bounds, whatever else it gets wrong.
    const uint NumLODs = min(Desc.NumLODs, (uint)MESHLET_MAX_LODS);

    // An authored forced LOD means "this LOD, in every view", and with LODs off there is only LOD 0 to
    // pick. Both are fixed picks, so the instance publishes kNoSurfaceDesc and no view re-selects.
    const bool bSelectPerView = Cull.ForcedLODIndex < 0 && Pass().bUseLODs != 0u;

    uint LODIndex = 0u;
    if (Cull.ForcedLODIndex >= 0)
    {
        LODIndex = min((uint)Cull.ForcedLODIndex, NumLODs > 0u ? NumLODs - 1u : 0u);
    }
    else if (bSelectPerView)
    {
        // The primary camera's pick. GetCameraPosition() is this dispatch's scene buffer -- the primary --
        // so this is view 0's LOD by construction, and the loop below re-derives it identically.
        LODIndex = SelectSurfaceLODForView(Desc, CamPos, Cull.SphereBounds);
    }

    // Distance drives the cap because cascade membership is essentially distance: only the far cascades
    // reach past ShadowCoarseLODDistSq, and their texel pitch is what makes the sloppy LODs safe there.
    // Must match the CPU's ResolveShadowLOD exactly -- the two seed the same cull dispatch domain.
    const uint ShadowCap = (Scene().CullData.ShadowCoarseLODDistSq > 0.0 && DistSq >= Scene().CullData.ShadowCoarseLODDistSq)
                         ? (uint)MESHLET_MAX_COARSE_SHADOW_LOD : (uint)MESHLET_MAX_SHADOW_LOD;
    const int  MaxShadowLOD  = (int)min(NumLODs > 0u ? NumLODs - 1u : 0u, ShadowCap);
    const uint ShadowLODIndex = (uint)clamp((int)LODIndex + Scene().CullData.ShadowLODBias, 0, MaxShadowLOD);

    // A null header with a non-zero count would page-fault the meshlet cull, so gate the count on it
    // exactly as the CPU gather did. Carried as a FLAG rather than tested on the pointer, because the
    // pointer lives in the static payload and this decision is made before that is worth loading.
    const bool bHasGeometry = Cull.HasFlag(EInstanceFlags::HasGeometry);

    const uint SurfaceMeshletOffset = Desc.LODMeshletOffset[LODIndex];
    const uint SurfaceMeshletCount  = bHasGeometry ? Desc.LODMeshletCount[LODIndex] : 0u;
    const uint ShadowMeshletOffset  = Desc.LODMeshletOffset[ShadowLODIndex];
    const uint ShadowMeshletCount   = bHasGeometry ? Desc.LODMeshletCount[ShadowLODIndex] : 0u;

    if (SurfaceMeshletCount == 0u && ShadowMeshletCount == 0u)
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
    const bool bCaster  = Cull.HasFlag(EInstanceFlags::CastShadow);
    const uint DrawID   = Cull.DrawID;
    const bool bInRange = DrawID < Pass().NumBatches;
    bool bAnyView = false;

    // The meshlet cull's per-instance thread domain, accumulated over the views that can actually emit:
    // one walk has to cover the widest LOD any of them picked, plus the shadow LOD. Seeded from the shadow
    // count because a shadow view needs no visibility of its own to be reached.
    uint WalkCount = ShadowMeshletCount;

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
            // The cascade's own ortho box (tested just above) is fitted to a sphere around the camera
            // sub-frustum slice, and those spheres NEST -- cascade 3's box swallows cascades 0-2 whole. The
            // box therefore rejects almost nothing between cascades, which is what made every caster reserve
            // and raster in all four. CascadeFrustum is the slice itself swept toward the sun: the actual set
            // of casters that can shade something this cascade is responsible for.
            //
            // Only the frustum half of the cascade cull runs here. The micro-poly reject stays meshlet-side:
            // this reservation is an upper bound the meshlet cull emits against, and a bound derived from a
            // radius the meshlet pass does not use verbatim could come up short and silently drop meshlets.
            if (View.HasFlag(CULL_VIEW_FLAG_CASCADE) &&
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

        // Non-shadow views pick their own LOD, so what this one reserves is what IT will walk -- a distant
        // capture reserves for its coarse LOD while the primary reserves for its fine one, out of the same
        // instance. Reserving the primary's count for everyone is what made every view inherit its LOD.
        uint Emitted = ShadowMeshletCount;
        if (!bShadowView)
        {
            Emitted = bSelectPerView
                ? (bHasGeometry ? SelectViewMeshletRange(Desc, View.ViewOrigin, Cull.SphereBounds).y : 0u)
                : SurfaceMeshletCount;
        }
        else if (bSelectPerView && View.HasFlag(CULL_VIEW_FLAG_CASCADE))
        {
            // Per-cascade shadow LOD. Cascade i's texel is wider than cascade i-1's at the same atlas
            // resolution, so it steps one LOD coarser; cascade 0 reproduces the single shadow pick above
            // exactly. CullMeshlets calls the same function with the same operands when it walks.
            Emitted = bHasGeometry
                ? SelectCascadeShadowMeshletRange(Desc, CamPos, Cull.SphereBounds, View.CascadeIndex,
                                                  Scene().CullData.ShadowLODBias,
                                                  Scene().CullData.ShadowCoarseLODDistSq).y
                : 0u;
        }

        // Every view that can emit, shadow ones included. Cascade 0 resolves to ShadowMeshletCount (which
        // already seeded WalkCount) and the rest are coarser, so today this never raises the domain -- it
        // is here so that a future cascade pick which IS finer cannot silently outrun the walk.
        WalkCount = max(WalkCount, Emitted);

        if (Emitted > 0u && bInRange)
        {
            InterlockedAdd(Pass().OutViewDrawCount[v * Pass().NumBatches + DrawID], Emitted);
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
        // The same per-instance number the prefix scan reads off the instance below: this is the cull's
        // dispatch domain and the prefix is what indexes into it, so the two have to agree or the tail of
        // the domain resolves nothing.
        InterlockedAdd(Pass().OutBatchMeshletCount[DrawID], WalkCount);
    }

    // ONLY survivors get here, which is the whole point of the split: the transform and the static
    // payload are 112 bytes that a rejected slot never touches. Assembled into the AoS output layout
    // every downstream pass already reads per meshlet / vertex / pixel.
    FInstanceStatic Static = Pass().RetainedStatic[SlotIdx];

    FGPUInstance Inst;
    Inst.Transform               = Pass().RetainedTransforms[SlotIdx];   // straight copy, no expansion
    Inst.SphereBounds            = Cull.SphereBounds;
    Inst.MeshletHeader           = Static.MeshletHeader;
    Inst.DrawID                  = DrawID;
    Inst.Flags                   = Cull.Flags;
    Inst.CustomData              = Static.CustomData;
    Inst.MaterialIndex           = Static.MaterialIndex;
    Inst.EntityID                = Static.EntityID;
    Inst.BoneOffset              = Static.BoneOffset;
    Inst.SkinnedVertexBase       = Static.SkinnedVertexBase;
    Inst.ShadowSkinnedVertexBase = Static.ShadowSkinnedVertexBase;
    Inst.SurfaceMeshletOffset    = SurfaceMeshletOffset;
    Inst.SurfaceMeshletCount     = SurfaceMeshletCount;
    Inst.ShadowMeshletOffset     = ShadowMeshletOffset;
    Inst.ShadowMeshletCount      = ShadowMeshletCount;
    Inst.SurfaceDescIndex        = bSelectPerView ? Cull.SurfaceDescIndex : kNoSurfaceDesc;
    Inst.MeshletWalkCount        = WalkCount;
    // End of the last LOD: the bound on the mesh-global meshlet index the cull emits. NumLODs is already
    // clamped to the table size above, so this cannot read past the desc.
    Inst.MeshletTotalCount       = bHasGeometry && NumLODs > 0u
                                 ? Desc.LODMeshletOffset[NumLODs - 1u] + Desc.LODMeshletCount[NumLODs - 1u]
                                 : 0u;

    Pass().OutInstances[OutSlot] = Inst;
}
