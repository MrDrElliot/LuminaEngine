#include "Includes/AppendBuffer.slang"
#include "Includes/Culling.slang"

// The reject set and the LOD pick must match BuildMeshletBlocks exactly.

#define LOCAL_SIZE_X 64

struct FCullInstancesArgs
{
    uint  NumViews;
    uint  NumBatches;
    uint  bUseLODs;
    // A skinned slot whose side data lacks this tag was not gathered this frame and must not be emitted.
    uint  SkinFrameTag;

    // Parallel arrays split by access rate; every lane streams CullEntries, only survivors read the other two.
    GPUSpan<FInstanceCullEntry> RetainedCullEntries;
    GPUSpan<FTransform3x4>      RetainedTransforms;
    GPUSpan<FInstanceStatic>    RetainedStatic;
    GPUSpan<FSurfaceDesc>       SurfaceDescs;

    GPUSpan<FGPUInstance>      OutInstances;
    GPUSpan<uint>              OutInstanceCount;
    // Indexed OutSlot * NumViews + v, the flat domain BuildMeshletBlocks dispatches over.
    GPUSpan<uint2>             OutInstanceViewRanges;
    // Only the two capacities are accumulated here; BuildDrawPrefix owns every other field.
    GPUSpan<FRenderBucket>     OutBuckets;
    GPUSpan<uint>              OutOverflowFlag;
    // Indexed by retained slot, so it is readable before compaction exists.
    GPUSpan<FSkinnedFrameData> SkinnedFrameData;
    // Only its Count is read here; Skinning.slang owns the writes.
    GPUSpan<FPreSkinnedVertex> PreSkinArena;
    // Runs past capacity by design, since the end value is the demand the CPU grows the buffer from.
    GPUSpan<uint>              OutPreSkinCursor;
    // One per workgroup of consecutive retained slots; empty disables the skip.
    GPUSpan<FInstanceBlockBounds> BlockBounds;
};
FCullInstancesArgs* Pass() { return GetPassArgs<FCullInstancesArgs>(); }

// Must reject the same blocks as Skinning.slang's per-workgroup skip, or the draw path reads unwritten pre-skin memory.
uint ClaimPreSkinSlice(FMeshletHeader* Header, uint MeshletOffset, uint MeshletCount, uint MeshletTotal)
{
    if (MeshletCount == 0u)
    {
        return kNoPreSkinBase;
    }

    // Whole-block reject bounded by the header's own count, since a partial slice is worse than inline skinning.
    if (MeshletOffset + MeshletCount > min(MeshletTotal, Header.MeshletCount))
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

    const uint Slot = AppendReserve(Pass().OutPreSkinCursor, Pass().PreSkinArena, /*Base*/ 0u,
                                    EndVertex - FirstVertex);
    if (Slot == kAppendFull)
    {
        return kNoPreSkinBase;
    }

    // Fold in the span start so the writer and the reader can both index by the meshlet's own VertexOffset.
    return Slot - FirstVertex;
}

// Mirror of Lumina::FInstanceBlockBounds, the distance bounds of one workgroup's slots.
struct FInstanceBlockBounds
{
    float3 Min;
    float  Reach;
    float3 Max;
    uint   bAlwaysScan;
};

// Conservative, so a passing block still tests each of its instances.
bool IsBlockInReach(FInstanceBlockBounds Block, float3 CamPos)
{
    if (Block.bAlwaysScan != 0u)
    {
        return true;
    }
    if (Block.Reach < 0.0)
    {
        return false;
    }
    const float3 ToBox = clamp(CamPos, Block.Min, Block.Max) - CamPos;
    return dot(ToBox, ToBox) <= Block.Reach * Block.Reach;
}

// Thinned foliage keeps at least this share of its instances, which caps how wide the survivors grow.
static const float kMinThinningKeep = 1.0 / 16.0;

// Keyed on the exact position bits, so an instance that has not moved keeps its answer every frame.
float ThinningHash(float3 Position)
{
    const uint3 Bits = asuint(Position);
    uint H = (Bits.x * 0x8da6b343u) ^ (Bits.y * 0xd8163841u) ^ (Bits.z * 0xcb1ab31fu);
    H ^= H >> 16u;
    H *= 0x7feb352du;
    H ^= H >> 15u;
    H *= 0x846ca68bu;
    H ^= H >> 16u;
    return float(H >> 8u) * (1.0 / 16777216.0);
}

// Every lane tests every view, so the group stages them once rather than each lane streaming 128 bytes per view.
groupshared FCullView gsViews[MAX_CULL_VIEWS];

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(uint3 GlobalID : SV_DispatchThreadID, uint3 GroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID)
{
    // The group's slots are one block, and the same answer for every lane keeps the barrier below uniform.
    const uint BlockIdx = GroupID.x + GroupID.y * MAX_DISPATCH_AXIS;
    if (Pass().BlockBounds.Contains(BlockIdx) && !IsBlockInReach(Pass().BlockBounds.Get(BlockIdx), GetCullCameraPosition()))
    {
        return;
    }

    // Ahead of every early return, since the whole group has to reach the barrier.
    const uint NumViews = min(Pass().NumViews, (uint)MAX_CULL_VIEWS);
    for (uint StageView = LocalID.x; StageView < NumViews; StageView += LOCAL_SIZE_X)
    {
        gsViews[StageView] = CullViews().GetAligned(StageView);
    }
    GroupMemoryBarrierWithGroupSync();

    // Undoes the Y fold the dispatch applies past MAX_DISPATCH_AXIS groups.
    const uint SlotIdx = GlobalID.x + GroupID.y * MAX_DISPATCH_AXIS * LOCAL_SIZE_X;
    if (!Pass().RetainedCullEntries.Contains(SlotIdx))
    {
        return;
    }

    // The only load on the reject path, 32 bytes and two slots per cache line.
    FInstanceCullEntry Cull = Pass().RetainedCullEntries.Get(SlotIdx);

    if (!Cull.HasFlag(EInstanceFlags::Active))
    {
        return;   // the payload beside a freed slot is stale by design
    }

    const bool bSkinned = Cull.HasFlag(EInstanceFlags::Skinned);

    FSkinnedFrameData Skin = (FSkinnedFrameData)0;
    if (bSkinned)
    {
        if (!Pass().SkinnedFrameData.Contains(SlotIdx))
        {
            return;
        }

        // The array is never cleared, so the tag separates this frame's gather from stale ranges.
        Skin = Pass().SkinnedFrameData.Get(SlotIdx);
        if (!Skin.IsFromFrame(Pass().SkinFrameTag))
        {
            return;
        }
    }

    float3       Center = Cull.SphereBounds.xyz;
    float        Radius = Cull.SphereBounds.w;
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

    // Thinned foliage keeps a share falling with distance squared, so the count on screen stays level, and every view sees the same choice.
    FTransform3x4 Transform = (FTransform3x4)0;
    bool bWidened = false;
    const float ThinningStart = Cull.ThinningStartDistance;
    if (!bNoCull && ThinningStart > 0.0 && DistSq > ThinningStart * ThinningStart)
    {
        const float Keep = max(ThinningStart * ThinningStart / DistSq, kMinThinningKeep);
        if (ThinningHash(Center) >= Keep)
        {
            return;
        }

        // Widened across the ground only, so a blade covers more without standing taller.
        const float  Widen = rsqrt(Keep);
        Transform = Pass().RetainedTransforms.Get(SlotIdx);
        bWidened  = true;
        const float3 Pivot = float3(Transform.Row0.w, Transform.Row1.w, Transform.Row2.w);
        Transform.Row0.xyz *= Widen;
        Transform.Row2.xyz *= Widen;
        Center = Pivot + (Center - Pivot) * float3(Widen, 1.0, Widen);
        Radius *= Widen;
    }

    // Past the table this reads garbage counts that BuildMeshletBlocks would append against unbounded.
    if (!Pass().SurfaceDescs.Contains(Cull.SurfaceDescIndex))
    {
        return;
    }

    FSurfaceDesc Desc = Pass().SurfaceDescs.Get(Cull.SurfaceDescIndex);

    // Bounds every LODMeshlet* index below, whatever else the table gets wrong.
    const uint NumLODs = min(Desc.NumLODs, (uint)MESHLET_MAX_LODS);

    // Never per-view for skinned, since the pre-skin slice was assigned against the CPU's LOD pick.
    const bool bSelectPerView = !bSkinned && Cull.ForcedLODIndex < 0 && Pass().bUseLODs != 0u;

    // A per-view instance starts from LOD 0 here only to test it has geometry; its picks come after visibility.
    const uint FixedLODIndex = Cull.ForcedLODIndex >= 0 ? min((uint)Cull.ForcedLODIndex, NumLODs > 0u ? NumLODs - 1u : 0u) : 0u;

    // A flag rather than a null-pointer test, since a slot that rejects here never loads the static payload.
    const bool bHasGeometry = Cull.HasFlag(EInstanceFlags::HasGeometry);

    // Skinned ranges already fold in the header-resident test, so bHasGeometry is not applied again.
    uint SurfaceMeshletOffset = bSkinned ? Skin.SurfaceMeshletOffset : Desc.LODMeshletOffset[FixedLODIndex];
    uint SurfaceMeshletCount  = bSkinned ? Skin.SurfaceMeshletCount
                                         : (bHasGeometry ? Desc.LODMeshletCount[FixedLODIndex] : 0u);

    if (SurfaceMeshletCount == 0u)
    {
        return;
    }

    // No early break, since every view the instance is visible to has to reserve room.
    const bool bCaster     = Cull.HasFlag(EInstanceFlags::CastShadow);
    const bool bShadowOnly = Cull.HasFlag(EInstanceFlags::ShadowOnly);
    const bool bFarCaster  = Cull.HasFlag(EInstanceFlags::CastFarShadow);
    const uint DrawID   = Cull.DrawID;
    const bool bInRange = DrawID < Pass().NumBatches;
    bool bAnyView = false;

    // A skinned instance needs a pre-skin slice only when some view emits it.
    bool bNeedsSkin = false;

    // End of the last LOD, the bound on every mesh-global meshlet index the cull emits.
    const uint LODMeshletTotal = bSkinned ? Skin.MeshletTotalCount
                               : (bHasGeometry && NumLODs > 0u
                                   ? Desc.LODMeshletOffset[NumLODs - 1u] + Desc.LODMeshletCount[NumLODs - 1u]
                                   : 0u);

    // Needed before the compacted slot is claimed, because that slot indexes the range slab.
    for (uint v = 0; v < NumViews; ++v)
    {
        if (IsInstanceVisibleToView(gsViews[v], Center, Radius, bNoCull, bCaster, bShadowOnly, bFarCaster))
        {
            bAnyView = true;
            break;
        }
    }

    if (!bAnyView)
    {
        return;
    }

    // Order is arbitrary since the draw list carries InstanceID explicitly.
    const uint OutSlot = AppendReserve(Pass().OutInstanceCount, Pass().OutInstances, /*Base*/ 0u);

    if (OutSlot == kAppendFull)
    {
        // The CPU grows the buffer from the read-back count, so this costs one frame rather than a write past the allocation.
        Pass().OutOverflowFlag.Set(0u, 1u);
        return;
    }

    // Reached, not copied, since a local copy puts a PhysicalStorageBuffer pointer in an OpVariable without Aliased or Restrict (invalid SPIR-V).
    FInstanceStatic* Static = Pass().RetainedStatic.At(SlotIdx);

    // Bounded by the header's own count, since a run no mesh can back becomes BuildMeshletBlocks' trip count and hangs the GPU.
    const uint MeshletTotal = min(LODMeshletTotal, Static.MeshletHeader.MeshletCount);

    if (!bWidened)
    {
        Transform = Pass().RetainedTransforms.Get(SlotIdx);
    }
    const float MaxScale = TransformMaxScale(Transform);

    // The primary camera's pick, which the instance carries for anything that reads it outside a view.
    if (bSelectPerView && bHasGeometry && NumViews > 0u)
    {
        const uint2 CameraRange = SelectViewMeshletRange(Desc, gsViews[0], Cull.SphereBounds, MaxScale);
        SurfaceMeshletOffset = CameraRange.x;
        SurfaceMeshletCount  = CameraRange.y;
    }

    // The single per-(instance, view) decision, read by BuildMeshletBlocks so the reservation and the append cannot disagree.
    for (uint v = 0; v < NumViews; ++v)
    {
        FCullView View = gsViews[v];

        uint2 Range   = uint2(0u, 0u);
        uint  Emitted = 0u;

        if (IsInstanceVisibleToView(View, Center, Radius, bNoCull, bCaster, bShadowOnly, bFarCaster))
        {
            if (bSelectPerView)
            {
                Range = bHasGeometry ? SelectViewMeshletRange(Desc, View, Cull.SphereBounds, MaxScale) : uint2(0u, 0u);
            }
            else
            {
                Range = uint2(SurfaceMeshletOffset, SurfaceMeshletCount);
            }

            // Clamped here, so the builder inherits a range that is already in bounds.
            Emitted = (Range.y == 0u || Range.x >= MeshletTotal || !bInRange)
                    ? 0u
                    : min(Range.y, MeshletTotal - Range.x);

            bNeedsSkin = bNeedsSkin || Emitted > 0u;
        }

        if (Emitted > 0u)
        {
            FRenderBucket* Bucket = Pass().OutBuckets.At(v * Pass().NumBatches + DrawID);

            // One reservation per emitted meshlet, since every pass that rasterizes the batch draws the same list.
            InterlockedAdd(Bucket.DrawCapacity, Emitted);

            // Rounded up per instance because blocks never span instances.
            const uint Blocks = (Emitted + (MESHLET_CULL_GROUP_SIZE - 1u)) / MESHLET_CULL_GROUP_SIZE;
            // Doubled for a two-phase view, whose early-drawn instances are built again at the back for the late re-test.
            InterlockedAdd(Bucket.BlockCapacity, View.HasFlag(CULL_VIEW_FLAG_MESHLET_HIZ) ? Blocks * 2u : Blocks);
        }

        // Written for every view, zero included, or the builder would read last frame's range.
        Pass().OutInstanceViewRanges.Set(OutSlot * Pass().NumViews + v, uint2(Range.x, Emitted));
    }

    uint SkinBase = kNoPreSkinBase;

    // Slot 0 is the null header, so this asks whether the header describes geometry.
    if (bSkinned && Static.MeshletHeader.MeshletCount != 0u)
    {
        // An unclaimed slice is not dropped, since the draw path blends its bones inline instead.
        if (bNeedsSkin)
        {
            SkinBase = ClaimPreSkinSlice(Static.MeshletHeader, SurfaceMeshletOffset, SurfaceMeshletCount, MeshletTotal);
        }

        // Written back so the skinning passes, keyed by retained slot, see the base the draw path reads off the compacted instance.
        Pass().SkinnedFrameData.At(SlotIdx).SkinnedVertexBase = SkinBase;
    }

    // Safe to assemble in a local because FGPUInstance holds a header slot, not an address.
    FGPUInstance Inst            = (FGPUInstance)0;
    Inst.Transform               = Transform;
    Inst.SphereBounds            = float4(Center, Radius);
    Inst.MeshletHeaderSlot       = Static.MeshletHeaderSlot;
    Inst.MeshletVisibilityBase   = Static.MeshletVisibilityBase;
    Inst.DrawID                  = DrawID;
    Inst.Flags                   = Cull.Flags;
    Inst.CustomData              = Static.CustomData;
    Inst.MaterialIndex           = Static.MaterialIndex;
    Inst.EntityID                = Static.EntityID;
    // Per frame for skinned, since the compacted arena base cannot live in the retained payload.
    Inst.BoneOffset              = bSkinned ? Skin.BoneOffset : Static.BoneOffset;
    Inst.SkinnedVertexBase       = bSkinned ? SkinBase : Static.SkinnedVertexBase;
    Inst.SurfaceMeshletOffset    = SurfaceMeshletOffset;
    Inst.SurfaceMeshletCount     = SurfaceMeshletCount;
    Inst.SurfaceDescIndex        = bSelectPerView ? Cull.SurfaceDescIndex : kNoSurfaceDesc;
    Inst.MeshletTotalCount       = MeshletTotal;
    // Survives compaction, unlike OutSlot, and keys the two-phase visibility flags.
    Inst.RetainedSlot            = SlotIdx;

    Pass().OutInstances.Set(OutSlot, Inst);
}
