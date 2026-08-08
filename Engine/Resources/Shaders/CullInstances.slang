#include "Includes/AppendBuffer.slang"
#include "Includes/Culling.slang"

// Instance-level cull, per-view LOD pick and compaction over the retained instance array. Also
// accumulates the per-(view, draw) meshlet totals BuildDrawPrefix turns into the draw-list layout.
// The reject set and the LOD pick must match BuildMeshletBlocks exactly.

#define LOCAL_SIZE_X 64

struct FCullInstancesArgs
{
    uint  NumRetained;        // slots in the retained array (dense; free slots carry bActive = 0)
    uint  NumViews;
    uint  NumBatches;
    uint  bUseLODs;

    uint  MaxVisibleInstances;
    uint  NumSurfaceDescs;

    // Split by ACCESS RATE: every lane streams CullEntries, the other two are read only by survivors.
    FInstanceCullEntry* RetainedCullEntries;
    FTransform3x4*      RetainedTransforms;
    FInstanceStatic*    RetainedStatic;
    FSurfaceDesc*       SurfaceDescs;

    FGPUInstance*      OutInstances;
    uint*              OutInstanceCount;
    // NumViews * NumBatches entries. This pass only accumulates the two capacities, which are the exact
    // upper bound on what each pair can emit and the same work counted in cull blocks; BuildDrawPrefix
    // turns them into the layout and owns every other field.
    FRenderBucket*     OutBuckets;
    uint*              OutOverflowFlag;
};
FCullInstancesArgs* Pass() { return GetPassArgs<FCullInstancesArgs>(); }

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(uint3 GlobalID : SV_DispatchThreadID, uint3 GroupID : SV_GroupID)
{
    // Undoes the Y fold the dispatch applies past MAX_DISPATCH_AXIS groups.
    const uint SlotIdx = GlobalID.x + GroupID.y * MAX_DISPATCH_AXIS * LOCAL_SIZE_X;
    if (SlotIdx >= Pass().NumRetained)
    {
        return;
    }

    // The only load on the reject path: 32 bytes, two slots per cache line.
    FInstanceCullEntry Cull = Pass().RetainedCullEntries[SlotIdx];

    if (!Cull.HasFlag(EInstanceFlags::Active))
    {
        return;   // freed slot; the payload beside it is stale by design
    }

    // CPU-fed at the head of the visible buffer; the retained copy has no valid bone offset, so
    // appending it would draw a garbage duplicate.
    if (Cull.HasFlag(EInstanceFlags::Skinned))
    {
        return;
    }

    const float3 Center = Cull.SphereBounds.xyz;
    const float  Radius = Cull.SphereBounds.w;
    const bool   bNoCull = Cull.HasFlag(EInstanceFlags::NoCull);

    // Cheapest reject, and the only view-independent one.
    const float3 CamPos = GetCullCameraPosition();
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

    // Past the table this reads whatever follows the allocation, and those counts become the block
    // counts BuildMeshletBlocks appends against: an unbounded append, not a visual artifact.
    if (Cull.SurfaceDescIndex >= Pass().NumSurfaceDescs)
    {
        return;
    }

    FSurfaceDesc Desc = Pass().SurfaceDescs[Cull.SurfaceDescIndex];

    // Bounds every LODMeshlet* index below, whatever else the table gets wrong.
    const uint NumLODs = min(Desc.NumLODs, (uint)MESHLET_MAX_LODS);

    // A forced LOD or LODs-off is a fixed pick, so the instance publishes kNoSurfaceDesc and no view
    // re-selects.
    const bool bSelectPerView = Cull.ForcedLODIndex < 0 && Pass().bUseLODs != 0u;

    uint LODIndex = 0u;
    if (Cull.ForcedLODIndex >= 0)
    {
        LODIndex = min((uint)Cull.ForcedLODIndex, NumLODs > 0u ? NumLODs - 1u : 0u);
    }
    else if (bSelectPerView)
    {
        LODIndex = SelectSurfaceLODForView(Desc, CamPos, Cull.SphereBounds);
    }

    // Must match the CPU's ResolveShadowLOD exactly: the two seed the same cull dispatch domain.
    const uint ShadowCap = (Scene().CullData.ShadowCoarseLODDistSq > 0.0 && DistSq >= Scene().CullData.ShadowCoarseLODDistSq)
                         ? (uint)MESHLET_MAX_COARSE_SHADOW_LOD : (uint)MESHLET_MAX_SHADOW_LOD;
    const int  MaxShadowLOD  = (int)min(NumLODs > 0u ? NumLODs - 1u : 0u, ShadowCap);
    const uint ShadowLODIndex = (uint)clamp((int)LODIndex + Scene().CullData.ShadowLODBias, 0, MaxShadowLOD);

    // A flag rather than a null-pointer test: the pointer lives in the static payload, which a slot
    // that rejects here never loads.
    const bool bHasGeometry = Cull.HasFlag(EInstanceFlags::HasGeometry);

    const uint SurfaceMeshletOffset = Desc.LODMeshletOffset[LODIndex];
    const uint SurfaceMeshletCount  = bHasGeometry ? Desc.LODMeshletCount[LODIndex] : 0u;
    const uint ShadowMeshletOffset  = Desc.LODMeshletOffset[ShadowLODIndex];
    const uint ShadowMeshletCount   = bHasGeometry ? Desc.LODMeshletCount[ShadowLODIndex] : 0u;

    if (SurfaceMeshletCount == 0u && ShadowMeshletCount == 0u)
    {
        return;   // nothing to raster; keep it out of the prefix domain entirely
    }

    // Per-view visibility AND per-(view, draw) reservation in one pass. No early break: every view the
    // instance is visible to has to reserve room, which is what buys an exactly sized draw list.
    const bool bCaster  = Cull.HasFlag(EInstanceFlags::CastShadow);
    const uint DrawID   = Cull.DrawID;
    const bool bInRange = DrawID < Pass().NumBatches;
    bool bAnyView = false;

    // End of the last LOD: the bound on every mesh-global meshlet index the cull emits, and the value
    // BuildMeshletBlocks clamps its own range against.
    const uint MeshletTotal = bHasGeometry && NumLODs > 0u
                            ? Desc.LODMeshletOffset[NumLODs - 1u] + Desc.LODMeshletCount[NumLODs - 1u]
                            : 0u;

    for (uint v = 0; v < Pass().NumViews; ++v)
    {
        FCullView View = CullViews()[v];

        if (!IsInstanceVisibleToView(View, Center, Radius, bNoCull, bCaster))
        {
            continue;
        }
        bAnyView = true;

        const bool bShadowView = View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY);

        // Branch order, operands and clamp all mirror BuildMeshletBlocks. Reserving against a different
        // count than the builder appends hands one bucket room nobody uses and starves the next.
        uint2 Range;
        if (bShadowView && bSelectPerView && View.HasFlag(CULL_VIEW_FLAG_CASCADE))
        {
            Range = bHasGeometry
                  ? SelectCascadeShadowMeshletRange(Desc, CamPos, Cull.SphereBounds, View.CascadeIndex,
                                                    Scene().CullData.ShadowLODBias,
                                                    Scene().CullData.ShadowCoarseLODDistSq)
                  : uint2(0u, 0u);
        }
        else if (bShadowView)
        {
            Range = uint2(ShadowMeshletOffset, ShadowMeshletCount);
        }
        else if (bSelectPerView)
        {
            Range = bHasGeometry ? SelectViewMeshletRange(Desc, View.ViewOrigin, Cull.SphereBounds)
                                 : uint2(0u, 0u);
        }
        else
        {
            Range = uint2(SurfaceMeshletOffset, SurfaceMeshletCount);
        }

        const uint Emitted = (Range.y == 0u || Range.x >= MeshletTotal)
                           ? 0u
                           : min(Range.y, MeshletTotal - Range.x);


        if (Emitted > 0u && bInRange)
        {
            FRenderBucket* Bucket = Pass().OutBuckets + (v * Pass().NumBatches + DrawID);
            InterlockedAdd(Bucket.DrawCapacity, Emitted);

            // Rounded up PER INSTANCE: blocks never span instances, so a ceil of the sum would
            // under-reserve one block for every instance with a partial tail.
            InterlockedAdd(Bucket.BlockCapacity,
                           (Emitted + (MESHLET_TASK_GROUP_SIZE - 1u)) / MESHLET_TASK_GROUP_SIZE);
        }
    }

    if (!bAnyView)
    {
        return;
    }

    // Order is arbitrary: the draw list carries InstanceID explicitly.
    const uint OutSlot = AppendReserve(Pass().OutInstanceCount, Pass().MaxVisibleInstances, /*Base*/ 0u);

    if (OutSlot == kAppendFull)
    {
        // The CPU grows the buffer from the count it reads back, so this costs one frame rather than
        // a write past the allocation.
        Pass().OutOverflowFlag[0] = 1u;
        return;
    }

    // Only survivors get here: 112 bytes a rejected slot never touches.
    FInstanceStatic Static = Pass().RetainedStatic[SlotIdx];

    // Written THROUGH the pointer, never assembled in a local: a local FGPUInstance puts its
    // MeshletHeader in a Function OpVariable that SPIR-V requires to carry Aliased or Restrict and
    // Slang emits with neither. Invalid module, accepted by every driver, device-lost frames later.
    FGPUInstance* Inst           = Pass().OutInstances + OutSlot;
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
    Inst.MeshletTotalCount       = MeshletTotal;
}
