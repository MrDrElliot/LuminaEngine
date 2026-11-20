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
    // 1 keeps shadow levels out of this dispatch, 2 culls only them, against this frame's pages once its depth is final.
    uint            bDeferShadows;
    uint            _Pad0;
    uint            _Pad1;

    GPUSpan<FRenderBucket> Buckets;
    GPUSpan<FMeshletBlock> BlockList;
    // Read by both dispatches and written by neither, so the partition sees one set of flags. A missing buffer has count 0.
    GPUSpan<uint>          PrevVisibility;
    // Stamped with VisibilityTag by every visible block; identical stores need no atomic and no clear.
    GPUSpan<uint>          OutVisibility;
    // One word per block of an instance's camera LOD. Both phases read it, and only the late phase replaces it.
    GPUSpan<uint>          MeshletVisibility;
    // Word 3 is how many blocks BuildMeshletCullArgs gave each group.
    GPUSpan<uint4>         CullDispatch;
    GPUSpan<FSurfaceDesc>  SurfaceDescs;
    // Shadow phase only, a bit per draw whose casters the compute raster can draw. Empty keeps every caster in hardware.
    GPUSpan<uint>          ComputeRasterBatches;
    // The static layer's entries, then the dynamic layer's from VSM_COMPUTE_RASTER_CAPACITY, each with its own cursor. Empty only counts them.
    GPUSpan<uint2>         ComputeRasterList;
    GPUSpan<uint>          ComputeRasterCursors;
    // The Shadow Raster view's record, a word of compute-drawn levels then one of hardware-drawn levels per instance. Empty records nothing.
    GPUSpan<uint>          ComputeRasterDebug;
    // The camera's compute raster, draw-list slots in the order the cull took them, and a cursor of how many with the phase start after it. An empty list only counts, and empty cursors keep the camera in hardware.
    GPUSpan<uint>          CameraRasterList;
    GPUSpan<uint>          CameraRasterCursors;
};

bool HasBatchBit(GPUSpan<uint> Bits, uint DrawID)
{
    const uint Word = DrawID >> 5u;
    return Bits.Contains(Word) && ((Bits.Get(Word) >> (DrawID & 31u)) & 1u) != 0u;
}
FMeshletCullArgs* Pass() { return GetPassArgs<FMeshletCullArgs>(); }

// A meshlet whose sphere spans at most this many texels of its level each side, so no triangle's scan box can grow large.
static const float kComputeRasterMaxRadiusTexels = 32.0;
// Texels each of its triangles covers on average, past which a lane scanning one would hold its whole wave up.
static const float kComputeRasterMaxTriangleTexels = 16.0;

// A meshlet of a draw that keeps the bare mesh's shape, on a virtual shadow level, whose triangles are each a few texels.
bool IsComputeRastered(FMeshletCullContext Ctx, FCullView View, uint DrawID, uint MeshletIdx, FMeshletBoundsCache Cache)
{
    // Skinned casters measured slower in compute than in hardware, so only static geometry takes this path.
    if (!View.HasFlag(CULL_VIEW_FLAG_VIRTUAL_SHADOW) || Ctx.bSkinned || Pass().ComputeRasterCursors.Count < 2u
        || !HasBatchBit(Pass().ComputeRasterBatches, DrawID))
    {
        return false;
    }

    const float RadiusTexels = Cache.WorldRadius / max(VirtualShadowLevel(View.ShadowLevel).TexelWorld, 1.0e-6);
    if (RadiusTexels > kComputeRasterMaxRadiusTexels)
    {
        return false;
    }

    // The sphere's cross section shared among the triangles, which keeps a meshlet of a few large triangles in hardware.
    const FMeshlet M = loadAligned<16>(MeshletHeaders().At(Ctx.HeaderSlot).Meshlets + MeshletIdx);
    return RadiusTexels * RadiusTexels * PI <= kComputeRasterMaxTriangleTexels * float(max(M.TriangleCount, 1u));
}

// A camera meshlet spanning at most this many pixels each side, and how many pixels each of its triangles covers on average.
static const float kCameraRasterMaxRadiusPixels   = 32.0;
static const float kCameraRasterMaxTrianglePixels = 8.0;

// A primary camera meshlet the compute rasterizer can draw in place of the hardware, in a bucket whose whole demand fits.
bool IsCameraComputeRastered(FMeshletCullContext Ctx, uint Arg, uint DrawID, uint MeshletIdx, FMeshletBoundsCache Cache)
{
    if (Pass().CameraRasterCursors.IsEmpty() || Ctx.bSkinned || !HasBatchBit(Pass().ComputeRasterBatches, DrawID))
    {
        return false;
    }

    // The back slots only stay clear of the hardware draws while the front never runs short.
    const FRenderBucket Bucket = Pass().Buckets.GetAligned(Arg);
    if (!BucketFitsDemand(Bucket.DrawDemand, Bucket.DrawCapacity, Bucket.BlockDemand, Bucket.BlockCapacity))
    {
        return false;
    }

    // An orthographic camera has no meaningful pixel size here, and a sphere reaching behind the eye needs the hardware clipper.
    const float4x4 Proj = GetCameraProjection();
    const float    W    = mul(Proj, CameraViewPosition(Cache.WorldCenter)).w;
    if (Proj[3][3] != 0.0 || W - Cache.WorldRadius <= 1.0e-3)
    {
        return false;
    }

    const float RadiusPixels = Cache.WorldRadius / (W - Cache.WorldRadius) * 0.5 * float(GetScreenSize().y) * abs(Proj[1][1]);
    if (RadiusPixels > kCameraRasterMaxRadiusPixels)
    {
        return false;
    }

    // The sphere's cross section shared among the triangles, which keeps a meshlet of a few large triangles in hardware.
    const FMeshlet M = loadAligned<16>(MeshletHeaders().At(Ctx.HeaderSlot).Meshlets + MeshletIdx);
    return RadiusPixels * RadiusPixels * PI <= kCameraRasterMaxTrianglePixels * float(max(M.TriangleCount, 1u));
}

#if MESHLET_CULL_GROUP_SIZE > 32
#error "MESHLET_CULL_GROUP_SIZE must fit one wave, or the wave votes below cover only part of the group."
#endif

static const uint kBlockTwoPhase        = 1u;
static const uint kBlockWasVisible      = 2u;
static const uint kBlockDeferredShadow = 4u;
// The block build fell back to a whole slot, so every meshlet draws without the cluster cut.
static const uint kBlockForceAll        = 8u;

// A group culls up to MESHLET_CULL_GROUP_SIZE blocks, so a block holding one cube's meshlet no longer idles 31 lanes.
groupshared uint gsBlockArg[MESHLET_CULL_GROUP_SIZE];
groupshared uint gsBlockInstance[MESHLET_CULL_GROUP_SIZE];
groupshared uint gsBlockMeshletBase[MESHLET_CULL_GROUP_SIZE];
groupshared uint gsBlockPrevBits[MESHLET_CULL_GROUP_SIZE];
groupshared uint gsBlockFlags[MESHLET_CULL_GROUP_SIZE];
groupshared uint gsBlockStart[MESHLET_CULL_GROUP_SIZE + 1];
groupshared uint gsBlockVisibleBits[MESHLET_CULL_GROUP_SIZE];
groupshared FMeshletCullContext gsBlockContext[MESHLET_CULL_GROUP_SIZE];

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

// The block whose meshlet range holds Item, the last one starting at or before it.
uint FindBlockForItem(uint Item, uint NumBlocks)
{
    uint Lo = 0u;
    uint Hi = NumBlocks;
    while (Lo + 1u < Hi)
    {
        const uint Mid = (Lo + Hi) >> 1u;
        if (gsBlockStart[Mid] <= Item)
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
    const uint NumBlocks  = clamp(Pass().CullDispatch.IsEmpty() ? 1u : Pass().CullDispatch.Get(0u).w, 1u, (uint)MESHLET_CULL_GROUP_SIZE);

    // Undoes the Y fold BuildMeshletCullArgs applies past MAX_DISPATCH_AXIS groups.
    const uint GroupWork = GroupID.x + GroupID.y * (uint)MAX_DISPATCH_AXIS;

    // Lane b gathers block b's state, then every block's meshlets are shared out across all the lanes.
    uint ArgIdx   = 0u;
    uint Flags    = 0u;
    bool bHasWork = false;
    FMeshletBlock Block = (FMeshletBlock)0;
    if (ThreadID < NumBlocks && NumBuckets > 0u)
    {
        const uint WorkIndex = GroupWork * NumBlocks + ThreadID;
        ArgIdx = FindBucketForWork(WorkIndex, NumBuckets);

        const FRenderBucket Bucket = Pass().Buckets.GetAligned(ArgIdx);
        const uint BlockSlot = WorkIndex - Bucket.CullWorkBase;

        // The late phase of a split bucket culls only the blocks the late build appended at the region's end.
        const bool bBackRegion = bLate && Pass().bSplitPhases != 0u && CullViewHasFlag(ArgIdx / NumDraws, CULL_VIEW_FLAG_MESHLET_HIZ)
                              && BucketFitsDemand(Bucket.DrawDemand, Bucket.DrawCapacity, Bucket.BlockDemand, Bucket.BlockCapacity);

        // Bounded by what was appended, not what CullInstances reserved, since unwritten blocks hold last frame's ring.
        const uint Front         = min(Bucket.BlockCursor, Bucket.BlockCapacity);
        const uint BlocksWritten = bBackRegion ? min(Bucket.BlockBackCount, Bucket.BlockCapacity - Front) : Front;
        const bool bBlockInRange = BlockSlot < BlocksWritten;
        const uint BlockIdx      = bBackRegion ? (Bucket.BlockBase + Bucket.BlockCapacity - BlocksWritten + BlockSlot)
                                               : (Bucket.BlockBase + BlockSlot);

        // Single-phase views belong to the early dispatch, except shadow levels waiting on this frame's pages.
        const uint ViewIndex       = ArgIdx / NumDraws;
        const bool bTwoPhase       = CullViewHasFlag(ViewIndex, CULL_VIEW_FLAG_MESHLET_HIZ);
        const bool bDeferredShadow = Pass().bDeferShadows != 0u && IsDeferredShadowView(ViewIndex);
        const bool bShadowPhase    = Pass().bDeferShadows == 2u;
        bHasWork = bBlockInRange && (bShadowPhase ? bDeferredShadow : (!bDeferredShadow && (!bLate || bTwoPhase)));
        if (bHasWork)
        {
            Block = Pass().BlockList.Get(BlockIdx);
        }
        Flags = (bTwoPhase ? kBlockTwoPhase : 0u) | (bDeferredShadow ? kBlockDeferredShadow : 0u)
              | ((Block.PackedBaseAndCount & MESHLET_BLOCK_FORCE_ALL_BIT) != 0u ? kBlockForceAll : 0u);
    }

    FGPUInstance* Inst = Instances() + Block.InstanceID;
    const bool bTwoPhaseWork = bHasWork && (Flags & kBlockTwoPhase) != 0u;

    uint RetainedSlot = 0u;
    if (bTwoPhaseWork)
    {
        RetainedSlot = Inst.RetainedSlot;
        Flags |= Pass().PrevVisibility.Get(RetainedSlot) == (Pass().VisibilityTag - 1u) ? kBlockWasVisible : 0u;
    }

    // A block from the coarser-LOD overflow fallback is not one of the camera range's blocks, so it has no word.
    uint MeshletVisibilityWord = 0u;
    bool bHasMeshletVisibility = false;
    uint MeshletVisibilityBits = ~0u;
    // A cluster block keeps its meshlets' bits at their places in the run, so the word means the same whatever range the LOD picks.
    uint MeshletVisibilityShift = 0u;
    const uint DescIndex = Inst.SurfaceDescIndex;
    // A skinned mesh's bind-pose run bounds would cull meshlets its pose has moved.
    const bool bRuns = bHasWork && !FGPUInstance::HasFlag(Inst, EInstanceFlags::Skinned) && DescIndex != kNoSurfaceDesc && Pass().SurfaceDescs.Contains(DescIndex);
    if (bRuns && bTwoPhaseWork && Inst.MeshletVisibilityBase != kNoMeshletVisibility)
    {
        const uint FirstMeshlet = Pass().SurfaceDescs.At(DescIndex).LODMeshletOffset[0];
        if (Block.MeshletBase >= FirstMeshlet)
        {
            MeshletVisibilityWord  = Inst.MeshletVisibilityBase + Block.MeshletBase / MESHLET_CULL_GROUP_SIZE - FirstMeshlet / MESHLET_CULL_GROUP_SIZE;
            MeshletVisibilityShift = Block.MeshletBase % MESHLET_CULL_GROUP_SIZE;
            bHasMeshletVisibility  = Pass().MeshletVisibility.Contains(MeshletVisibilityWord);
            if (bHasMeshletVisibility)
            {
                MeshletVisibilityBits = Pass().MeshletVisibility.Get(MeshletVisibilityWord) >> MeshletVisibilityShift;
            }
        }
    }
    else if (bTwoPhaseWork && Inst.MeshletVisibilityBase != kNoMeshletVisibility && Block.MeshletBase >= Inst.SurfaceMeshletOffset)
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

    // Late only, because the early phase is a replay and an instance dropped there is dropped for good.
    bool bInstanceOccluded = false;
    if (bLate && bTwoPhaseWork && !FGPUInstance::HasFlag(Inst, EInstanceFlags::NoCull))
    {
        // The one occlusion test a skinned instance can take, since its sphere is re-fed every frame.
        const float4 Sphere = loadAligned<16>(&Inst.SphereBounds);
        const FCullSphereProjection Proj = ProjectCullSphere(Sphere.xyz, Sphere.w);

        // It leaves every meshlet invisible, so the flag clears for next frame.
        bInstanceOccluded = Proj.bValid && IsHiZOccluded(Proj);
    }

    // A block is one run of a cluster mesh, so a run the view or the cut takes nothing from skips all its meshlet tests.
    bool bRunCulled = false;
    if (bRuns && !bInstanceOccluded)
    {
        FMeshletHeader* Header = MeshletHeaders().At(Inst.MeshletHeaderSlot);
        if (Header.bClusterLOD)
        {
            const FCullView View = CullViews().GetAligned(ArgIdx / NumDraws);
            const bool bFrustum  = View.HasFlag(CULL_VIEW_FLAG_FRUSTUM) && !FGPUInstance::HasFlag(Inst, EInstanceFlags::NoCull);
            bRunCulled = IsMeshletRunCulled(Header, Block.MeshletBase / MESHLET_CULL_GROUP_SIZE, View, Inst.ModelMatrix, TransformMaxScale(Inst.Transform),
                                            Pass().SurfaceDescs.At(DescIndex).LODErrorMultiplier, bFrustum, (Flags & kBlockForceAll) == 0u);
        }
    }

    const uint Meshlets = (bHasWork && !bInstanceOccluded && !bRunCulled) ? Block.Count : 0u;
    const uint Start    = WavePrefixSum(Meshlets);
    const uint Total    = WaveActiveSum(Meshlets);
    if (ThreadID < NumBlocks)
    {
        gsBlockArg[ThreadID]         = ArgIdx;
        gsBlockInstance[ThreadID]    = Block.InstanceID;
        gsBlockMeshletBase[ThreadID] = Block.MeshletBase;
        gsBlockPrevBits[ThreadID]    = MeshletVisibilityBits;
        gsBlockFlags[ThreadID]       = Flags;
        gsBlockStart[ThreadID]       = Start;
        gsBlockVisibleBits[ThreadID] = 0u;
    }
    // Only a block with meshlets owns items, so only those need the context the item loop reads.
    if (Meshlets > 0u)
    {
        FMeshletCullContext Ctx = MakeMeshletCullContext(Inst);
        if ((Ctx.bSelectsLOD || Ctx.bCameraCut) && Pass().SurfaceDescs.Contains(Ctx.SurfaceDescIndex))
        {
            Ctx.LODErrorMultiplier = Pass().SurfaceDescs.At(Ctx.SurfaceDescIndex).LODErrorMultiplier;
        }
        gsBlockContext[ThreadID] = Ctx;
    }
    if (ThreadID == 0u)
    {
        gsBlockStart[NumBlocks] = Total;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint Base = 0u; Base < Total; Base += MESHLET_CULL_GROUP_SIZE)
    {
        const uint Item = Base + ThreadID;
        bool bKeep        = false;
        bool bComputeRaster = false;
        bool bComputeStatic = false;
        bool bShadowLevelKeep = false;
        bool bCameraCompute = false;
        uint ComputeLevel   = 0u;
        uint KeepArg      = 0u;
        uint KeepInstance = 0u;
        uint KeepMeshlet  = 0u;
        if (Item < Total)
        {
            const uint Owner      = FindBlockForItem(Item, NumBlocks);
            const uint Local      = Item - gsBlockStart[Owner];
            const uint OwnerFlags = gsBlockFlags[Owner];
            KeepArg      = gsBlockArg[Owner];
            KeepInstance = gsBlockInstance[Owner];
            KeepMeshlet  = gsBlockMeshletBase[Owner] + Local;

            const bool bTwoPhase          = (OwnerFlags & kBlockTwoPhase) != 0u;
            const bool bWasMeshletVisible = (OwnerFlags & kBlockWasVisible) != 0u && ((gsBlockPrevBits[Owner] >> Local) & 1u) != 0u;

            // Early emits last frame's visible meshlets and late their complement, so each rasterizes once whatever the bits hold.
            const bool bEmitPhase = !bTwoPhase || (bLate != bWasMeshletVisible);

            // Late also tests what early drew, since that test is what writes next frame's flag and bits.
            if (bLate || bEmitPhase)
            {
                const FMeshletCullContext Ctx = gsBlockContext[Owner];
                // Blocks of a group nearly always share a view, so this load hits one address across the wave.
                const FCullView View          = CullViews().GetAligned(KeepArg / NumDraws);
                FMeshletBoundsCache Cache     = MakeMeshletBoundsCache();

                // No Hi-Z in the early replay, since the late phase never re-emits what the early phase drew.
                bool bVisible = TestMeshletAgainstView(Ctx, View, KeepMeshlet, Cache, bLate, (OwnerFlags & kBlockForceAll) != 0u);
                if (bVisible && (OwnerFlags & kBlockDeferredShadow) != 0u && Ctx.bPosedBounds)
                {
                    bVisible = !IsShadowLevelCasterCulled(View, Cache.WorldCenter, Cache.WorldRadius);
                }

                bKeep = bEmitPhase && bVisible;
                bComputeRaster = bKeep && IsComputeRastered(Ctx, View, KeepArg % NumDraws, KeepMeshlet, Cache);
                bComputeStatic = View.HasFlag(CULL_VIEW_FLAG_SHADOW_CACHE_STATIC);
                ComputeLevel   = View.ShadowLevel;
                bShadowLevelKeep = bKeep && View.HasFlag(CULL_VIEW_FLAG_VIRTUAL_SHADOW);
                bCameraCompute   = bKeep && KeepArg < NumDraws && IsCameraComputeRastered(Ctx, KeepArg, KeepArg, KeepMeshlet, Cache);
                if (bVisible)
                {
                    InterlockedOr(gsBlockVisibleBits[Owner], 1u << Local);
                }
            }
        }

        // A full list leaves the meshlet to the hardware draw, so nothing is lost when a frame outgrows it. One branch per layer keeps each append on one cursor.
        const uint2 ComputeEntry  = uint2(KeepInstance, (KeepMeshlet << VSM_COMPUTE_RASTER_LEVEL_BITS) | ComputeLevel);
        const uint  LayerCapacity = Pass().ComputeRasterList.Count >> 1u;
        if (bComputeRaster && bComputeStatic)
        {
            const uint Slot = AppendReserve(Pass().ComputeRasterCursors.At(0u), LayerCapacity, 0u);
            if (Slot != kAppendFull)
            {
                Pass().ComputeRasterList.Set(Slot, ComputeEntry);
                bKeep = false;
            }
        }
        if (bComputeRaster && !bComputeStatic)
        {
            const uint Slot = AppendReserve(Pass().ComputeRasterCursors.At(1u), LayerCapacity, VSM_COMPUTE_RASTER_CAPACITY);
            if (Slot != kAppendFull)
            {
                Pass().ComputeRasterList.Set(Slot, ComputeEntry);
                bKeep = false;
            }
        }

        // The work entry is taken first, since a back slot once claimed cannot be handed back to the hardware draw.
        if (bCameraCompute)
        {
            const uint Work = AppendReserve(Pass().CameraRasterCursors.At(0u), Pass().CameraRasterList.Count, 0u);
            if (Work != kAppendFull)
            {
                FRenderBucket* Bucket = Pass().Buckets.At(KeepArg);
                const uint Back = WaveInterlockedAddKeyed(KeepArg, &Bucket.DrawBackCursor, 1u);
                uint Slot = kAppendFull;
                if (Back < Bucket.DrawCapacity)
                {
                    Slot = Bucket.DrawBase + Bucket.DrawCapacity - 1u - Back;
                    MeshletDrawList().Set(Slot, FMeshletDraw::Make(KeepInstance, KeepMeshlet, Scene().CullData.MeshletDrawTag));
                }
                Pass().CameraRasterList.Set(Work, Slot);
                bKeep = false;
            }
        }

        // A meshlet the compute raster took has bKeep cleared by now, so it lands in the compute word.
        if (bShadowLevelKeep && Pass().ComputeRasterDebug.Contains(KeepInstance * 2u + 1u))
        {
            InterlockedOr(Pass().ComputeRasterDebug.At(KeepInstance * 2u + (bKeep ? 1u : 0u))[0], 1u << ComputeLevel);
        }

        // One atomic per bucket the wave's survivors land in, which is nearly always one.
        bool bPending = bKeep;
        while (WaveActiveAnyTrue(bPending))
        {
            const uint Arg   = WaveActiveMin(bPending ? KeepArg : 0xFFFFFFFFu);
            const bool bMine = bPending && KeepArg == Arg;
            FRenderBucket* Bucket = Pass().Buckets.At(Arg);

            // The cursor still advances by the full count past capacity, so the readback keeps reporting demand.
            const uint Kept = WaveActiveCountBits(bMine);
            uint Old = 0u;
            if (bMine && WavePrefixCountBits(bMine) == 0u)
            {
                uint* Cursor = &Bucket.DrawCursor;
                InterlockedAdd(Cursor[0], Kept, Old);
            }
            Old = WaveActiveMax(Old);

            const uint Slot = Old + WavePrefixCountBits(bMine);
            if (bMine && Slot < Bucket.DrawCapacity)
            {
                MeshletDrawList().Set(Bucket.DrawBase + Slot, FMeshletDraw::Make(KeepInstance, KeepMeshlet, Scene().CullData.MeshletDrawTag));
            }
            bPending = bPending && !bMine;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (!bHasWork || bInstanceOccluded)
    {
        return;
    }
    const uint VisibleMeshlets = gsBlockVisibleBits[ThreadID];

    // This block owns its whole word, so a plain store replaces it.
    if (bLate && bHasMeshletVisibility)
    {
        Pass().MeshletVisibility.Set(MeshletVisibilityWord, VisibleMeshlets << MeshletVisibilityShift);
    }

    // Every visible block stores the same stamp, so an occluded block of the same instance cannot undo it.
    if (bLate && bTwoPhaseWork && VisibleMeshlets != 0u && Pass().OutVisibility.Contains(RetainedSlot))
    {
        Pass().OutVisibility.At(RetainedSlot)[0] = Pass().VisibilityTag;
    }
}
