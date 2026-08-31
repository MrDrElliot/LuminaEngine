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
    // Monotonic per rendered frame. A skinned slot whose side data does not carry this was not gathered, so
    // its ranges belong to some earlier frame and it must not be emitted.
    uint  SkinFrameTag;
    // Elements the pre-skinned vertex buffer holds. The claim below clamps against it; anything past it
    // gets kNoPreSkinBase and the draw path skins that block inline instead.
    uint  PreSkinCapacity;

    // Split by ACCESS RATE: every lane streams CullEntries, the other two are read only by survivors.
    FInstanceCullEntry* RetainedCullEntries;
    FTransform3x4*      RetainedTransforms;
    FInstanceStatic*    RetainedStatic;
    FSurfaceDesc*       SurfaceDescs;

    FGPUInstance*      OutInstances;
    uint*              OutInstanceCount;
    // (base, count) per (visible instance, view), indexed OutSlot * NumViews + v -- exactly the flat
    // domain BuildMeshletBlocks dispatches over. The single source of the per-view meshlet range.
    uint2*             OutInstanceViewRanges;
    // NumViews * NumBatches entries. This pass only accumulates the two capacities, which are the exact
    // upper bound on what each pair can emit and the same work counted in cull blocks; BuildDrawPrefix
    // turns them into the layout and owns every other field.
    FRenderBucket*     OutBuckets;
    uint*              OutOverflowFlag;
    // Per-frame half of a SKINNED slot's payload: the LOD ranges the CPU resolved and the pre-skin slices it
    // assigned against them. Indexed by RETAINED slot, so it is readable here, before compaction exists.
    FSkinnedFrameData* SkinnedFrameData;
    // Global pre-skin vertex cursor. Claimed here, beside the LOD pick that decides which vertices a block
    // even has, so the two cannot disagree -- they used to be a CPU allocation and a GPU pick held in step
    // only by a comment. Runs past capacity by design: what it ends at is DEMAND, which is what the CPU
    // grows the buffer from.
    uint*              OutPreSkinCursor;
};
FCullInstancesArgs* Pass() { return GetPassArgs<FCullInstancesArgs>(); }

// Claims one block's slice of the pre-skinned vertex buffer. The span is [first meshlet's VertexOffset,
// last meshlet's end), read through the header -- exactly what the CPU's BlockExtent used to walk the
// meshlet array for. Returns kNoPreSkinBase when the block has nothing to skin or the budget is spent, and
// the draw path then blends that block's bones inline instead.
// The claim and Skinning.slang's per-workgroup skip must reject the SAME blocks. A block that is claimed
// but not written leaves the draw path reading pre-skin memory nobody filled -- the reverse of an overflow,
// and silent.
uint ClaimPreSkinSlice(FMeshletHeader* Header, uint MeshletOffset, uint MeshletCount, uint MeshletTotal)
{
    if (MeshletCount == 0u)
    {
        return kNoPreSkinBase;
    }

    // Whole-block reject rather than a per-meshlet clamp: the skinning pass drops individual meshlets past
    // the total, and a partially written slice is worse than one that falls back to inline skinning.
    //
    // Bounded by the HEADER's own count as well as the instance's: MeshletTotal comes from the surface's
    // LOD table and the arrays being indexed come from the header, and only the header knows how long
    // they are. A mesh whose header describes no geometry has MeshletCount 0 and rejects here.
    if (MeshletOffset + MeshletCount > min(MeshletTotal, Header.MeshletCount))
    {
        return kNoPreSkinBase;
    }

    // Same gate TestMeshletAgainstView needs: a count over a null array has been seen in a GPU dump.
    if (Header.Meshlets == nullptr)
    {
        return kNoPreSkinBase;
    }

    const uint FirstVertex = Header.Meshlets[MeshletOffset].VertexOffset;
    const FMeshlet Last    = loadAligned<16>(Header.Meshlets + (MeshletOffset + MeshletCount - 1u));
    const uint EndVertex   = Last.VertexOffset + Last.VertexCount;

    if (EndVertex <= FirstVertex)
    {
        return kNoPreSkinBase;
    }

    const uint Slot = AppendReserve(Pass().OutPreSkinCursor, Pass().PreSkinCapacity, /*Base*/ 0u,
                                    EndVertex - FirstVertex);
    if (Slot == kAppendFull)
    {
        return kNoPreSkinBase;
    }

    // Fold in the span start so the writer and the reader can both index by the meshlet's own VertexOffset.
    return Slot - FirstVertex;
}

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

    // Skinned slots used to be rejected here and CPU-fed into the head of the visible buffer instead,
    // because FInstanceStatic could not describe them: its bone offset and pre-skin bases were per-frame
    // values. The bone offset is a stable arena base now, and the per-frame remainder arrives through
    // SkinnedFrameData below -- so they compact through this pass like everything else, which is what earns
    // them a RetainedSlot and therefore real two-phase occlusion.
    const bool bSkinned = Cull.HasFlag(EInstanceFlags::Skinned);

    FSkinnedFrameData Skin = (FSkinnedFrameData)0;
    if (bSkinned)
    {
        if (Pass().SkinnedFrameData == nullptr)
        {
            return;
        }

        // The array is never cleared, so the tag is what distinguishes "gathered this frame" from a slot
        // still holding a previous frame's ranges. An entity the CPU gather culled is simply not emitted.
        Skin = Pass().SkinnedFrameData[SlotIdx];
        if (!Skin.IsFromFrame(Pass().SkinFrameTag))
        {
            return;
        }
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
    // Never for skinned: the pre-skin slice was assigned against the CPU's pick, and a view that re-selected
    // a different LOD would pre-skin one LOD's meshlets while the raster read another's vertices.
    const bool bSelectPerView = !bSkinned && Cull.ForcedLODIndex < 0 && Pass().bUseLODs != 0u;

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

    // Skinned takes the CPU's resolved ranges verbatim, for the reason on bSelectPerView above. Those counts
    // already fold in the header-resident test, so bHasGeometry is not applied to them a second time.
    const uint SurfaceMeshletOffset = bSkinned ? Skin.SurfaceMeshletOffset : Desc.LODMeshletOffset[LODIndex];
    const uint SurfaceMeshletCount  = bSkinned ? Skin.SurfaceMeshletCount
                                               : (bHasGeometry ? Desc.LODMeshletCount[LODIndex] : 0u);
    const uint ShadowMeshletOffset  = bSkinned ? Skin.ShadowMeshletOffset : Desc.LODMeshletOffset[ShadowLODIndex];
    const uint ShadowMeshletCount   = bSkinned ? Skin.ShadowMeshletCount
                                               : (bHasGeometry ? Desc.LODMeshletCount[ShadowLODIndex] : 0u);

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

    // A skinned block only needs a pre-skin slice for the view class that actually reads it.
    bool bNeedsSurfaceSkin = false;
    bool bNeedsShadowSkin  = false;

    // End of the last LOD: the bound on every mesh-global meshlet index the cull emits.
    const uint LODMeshletTotal = bSkinned ? Skin.MeshletTotalCount
                               : (bHasGeometry && NumLODs > 0u
                                   ? Desc.LODMeshletOffset[NumLODs - 1u] + Desc.LODMeshletCount[NumLODs - 1u]
                                   : 0u);

    // Does ANY view see it? Breaks on the first hit, so a visible instance pays one or two iterations.
    // Needed before the compacted slot is claimed, because that slot is what indexes the range slab.
    for (uint v = 0; v < Pass().NumViews; ++v)
    {
        if (IsInstanceVisibleToView(CullViews()[v], Center, Radius, bNoCull, bCaster))
        {
            bAnyView = true;
            break;
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

    // REACHED, not copied: a local copy puts a PhysicalStorageBuffer pointer in an OpVariable carrying neither Aliased nor Restrict, which is an invalid module every driver accepts.
    FInstanceStatic* Static = Pass().RetainedStatic + SlotIdx;

    // Bounded by the header's own count: a run no mesh can back becomes BuildMeshletBlocks' trip count, which hangs the GPU.
    const uint MeshletTotal = min(LODMeshletTotal, Static.MeshletHeader.MeshletCount);

    // THE per-(instance, view) decision, made once. BuildMeshletBlocks reads what this writes instead of
    // re-deriving it, so the reservation and the append cannot disagree -- a mismatch used to hand one
    // bucket room nobody uses, starve the next, and drop the newest mesh with no error anywhere.
    for (uint v = 0; v < Pass().NumViews; ++v)
    {
        FCullView View = loadAligned<16>(CullViews() + v);

        uint2 Range   = uint2(0u, 0u);
        uint  Emitted = 0u;

        if (IsInstanceVisibleToView(View, Center, Radius, bNoCull, bCaster))
        {
            const bool bShadowView = View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY);

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

            // Clamped here, so the builder inherits a range that is already in bounds.
            Emitted = (Range.y == 0u || Range.x >= MeshletTotal || !bInRange)
                    ? 0u
                    : min(Range.y, MeshletTotal - Range.x);

            bNeedsSurfaceSkin = bNeedsSurfaceSkin || (Emitted > 0u && !bShadowView);
            bNeedsShadowSkin  = bNeedsShadowSkin  || (Emitted > 0u && bShadowView);
        }

        if (Emitted > 0u)
        {
            FRenderBucket* Bucket = Pass().OutBuckets + (v * Pass().NumBatches + DrawID);

            // One reservation per emitted meshlet, however many passes rasterize the batch. MeshletCull
            // appends each bucket's region exactly once per phase and every pass draws that same list,
            // so a second rasterization consumes no further slots -- which is what the old x2 translucent
            // reservation existed to cover.
            InterlockedAdd(Bucket.DrawCapacity, Emitted);

            // Rounded up PER INSTANCE: blocks never span instances, so a ceil of the sum would
            // under-reserve one block for every instance with a partial tail.
            InterlockedAdd(Bucket.BlockCapacity,
                           (Emitted + (MESHLET_CULL_GROUP_SIZE - 1u)) / MESHLET_CULL_GROUP_SIZE);
        }

        // Written for EVERY view, zero included. The builder indexes this slab directly from its flat
        // thread id, so a pair left unwritten would hand it last frame's range.
        Pass().OutInstanceViewRanges[OutSlot * Pass().NumViews + v] = uint2(Range.x, Emitted);
    }

    // Pre-skin slices, claimed from the SAME resolved ranges the emit above used. A block's vertex span is
    // the span of its meshlets, so it is read straight off the header rather than precomputed on the CPU.
    // SkinnedVertexBase folds in the span start, so base + M.VertexOffset lands in the compacted slice.
    uint SurfaceSkinBase = kNoPreSkinBase;
    uint ShadowSkinBase  = kNoPreSkinBase;

    // A header always exists -- slot 0 is the null header -- so this asks whether it describes geometry,
    // which is the question that was actually being asked before slots replaced addresses.
    if (bSkinned && Static.MeshletHeader.MeshletCount != 0u)
    {
        // A shadow block that resolved to the surface block shares its slice; claiming twice would both
        // waste the budget and skin the same vertices into two places.
        const bool bShareShadowBlock = (ShadowMeshletOffset == SurfaceMeshletOffset);

        if (bNeedsSurfaceSkin || (bNeedsShadowSkin && bShareShadowBlock))
        {
            SurfaceSkinBase = ClaimPreSkinSlice(Static.MeshletHeader, SurfaceMeshletOffset, SurfaceMeshletCount, MeshletTotal);
        }

        // An unclaimed block is not dropped: the draw path blends its bones inline instead.
        ShadowSkinBase = bShareShadowBlock
                       ? SurfaceSkinBase
                       : (bNeedsShadowSkin
                            ? ClaimPreSkinSlice(Static.MeshletHeader, ShadowMeshletOffset, ShadowMeshletCount, MeshletTotal)
                            : kNoPreSkinBase);

        // Written back so the skinning passes, which are keyed by RETAINED slot, see the same bases the
        // draw path will read off the compacted instance below.
        Pass().SkinnedFrameData[SlotIdx].SkinnedVertexBase       = SurfaceSkinBase;
        Pass().SkinnedFrameData[SlotIdx].ShadowSkinnedVertexBase = ShadowSkinBase;
    }

    // Safe to assemble in a local and store once now that FGPUInstance holds a header slot, not an address.
    FGPUInstance Inst            = (FGPUInstance)0;
    Inst.Transform               = Pass().RetainedTransforms[SlotIdx];   // straight copy, no expansion
    Inst.SphereBounds            = Cull.SphereBounds;
    Inst.MeshletHeaderSlot       = Static.MeshletHeaderSlot;
    Inst._HeaderPad0             = 0u;
    Inst.DrawID                  = DrawID;
    Inst.Flags                   = Cull.Flags;
    Inst.CustomData              = Static.CustomData;
    Inst.MaterialIndex           = Static.MaterialIndex;
    Inst.EntityID                = Static.EntityID;
    // Per-FRAME for skinned: the compacted arena base cannot live in the retained payload.
    Inst.BoneOffset              = bSkinned ? Skin.BoneOffset : Static.BoneOffset;
    // Claimed just above for skinned; kNoPreSkinBase routes the block to inline skinning in the draw path.
    Inst.SkinnedVertexBase       = bSkinned ? SurfaceSkinBase : Static.SkinnedVertexBase;
    Inst.ShadowSkinnedVertexBase = bSkinned ? ShadowSkinBase  : Static.ShadowSkinnedVertexBase;
    Inst.SurfaceMeshletOffset    = SurfaceMeshletOffset;
    Inst.SurfaceMeshletCount     = SurfaceMeshletCount;
    Inst.ShadowMeshletOffset     = ShadowMeshletOffset;
    Inst.ShadowMeshletCount      = ShadowMeshletCount;
    Inst.SurfaceDescIndex        = bSelectPerView ? Cull.SurfaceDescIndex : kNoSurfaceDesc;
    Inst.MeshletTotalCount       = MeshletTotal;
    // Survives compaction, which OutSlot does not: it is the key into the two-phase visibility flags.
    Inst.RetainedSlot            = SlotIdx;

    Pass().OutInstances[OutSlot] = Inst;
}
