#pragma once
#include "Includes/Culling.slang"

// Two-phase meshlet culling (Nanite-style), feeding the VisBuffer geometry pass.
//  Phase 0 (Early): frustum + cone every view; the camera view also tests Hi-Z occlusion against LAST
//                   frame's pyramid. Survivors emit to their view; camera meshlets the stale pyramid hides
//                   are DEFERRED (not dropped). The VisBuffer rasters the early camera view, then the
//                   pyramid is rebuilt from this frame's partial depth.
//  Phase 1 (Late):  re-test only the deferred meshlets against the rebuilt pyramid; ones now visible (the
//                   stale pyramid wrongly hid them -- disocclusion) emit to the camera-late view and the
//                   VisBuffer rasters them. This removes the one-frame disocclusion lag of a single phase.
#define LOCAL_SIZE_X 64

// Mesh-task indirect args {GroupCountX, Y, Z}. The cull writes GroupCountX directly (= surviving meshlets) so
// no separate VS-args -> mesh-args conversion pass is needed; Y/Z are pre-seeded to 1 each frame CPU-side.
struct FDrawMeshTasksIndirectArguments
{
    uint GroupCountX;
    uint GroupCountY;
    uint GroupCountZ;
};

struct FPushConstants
{
    uint NumViews;
    uint Phase;              // CULL_PHASE_EARLY or CULL_PHASE_LATE
    uint CameraLateViewIndex; // Only consumed in LATE; index into CullViews().
    // Entries SurfaceDescs actually holds (what the DEVICE buffer holds, not what the game thread has
    // interned). Bounds the per-view LOD lookup below.
    uint NumSurfaceDescs;
    // Cull-private scratch, addressed by device pointer (not a descriptor) so draw/post passes
    // never bind or transition them. State tracked CPU-side via State.Writes; 8-byte aligned here.
    FDrawIndirectArguments*          IndirectArgs;
    FMeshletDeferred*                DeferList;
    uint*                            DeferCount;
    FDrawMeshTasksIndirectArguments* MeshDrawArgs;   // GroupCountX accumulated here too (mesh path)
    // [0] = meshlet work domain (the dispatch domain), [1] = surviving instance count. Both are produced
    // by BuildDrawPrefix from GPU-side counts, so they cannot come through FSceneGlobalData any more.
    uint*                            Totals;
    // Per-(view, draw) emit bound, parallel to IndirectArgs. Each region in the draw list is sized to
    // exactly this, so a pair cannot spill into its neighbour and the bound below is exact rather than a
    // conservative slice end.
    uint*                            ViewDrawCounts;
    // Per-(view, draw) base into the draw list. Read from here rather than from
    // IndirectArgs[].FirstInstance: that struct's InstanceCount is being atomically hammered by every other
    // lane in this same dispatch, so loading the base out of it puts a read-only value on a contended
    // cacheline. This array is written once by BuildDrawPrefix and read-only for the rest of the frame.
    uint*                            ViewDrawOffsets;
    // The interned per-surface LOD tables, so each view can pick its own LOD from its own origin. Same
    // buffer CullInstances reserved draw-list space from, read through the same helper, so the two picks
    // cannot drift. Grouped with the other pointers so the trailing uint pair needs no padding.
    FSurfaceDesc*                    SurfaceDescs;
    // Entries the shared meshlet draw list can hold; outer backstop for a short allocation.
    uint                             DrawListCapacity;
    // Entries the defer list can hold. DeferCount is an unbounded atomic, so this is what stops the
    // append from writing past the allocation -- and what stops the late phase reading past it.
    uint                             DeferListCapacity;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Appends a surviving meshlet into the view's slice; centralized so both phases share the atomic protocol.
// MeshletIdx is MESH-GLOBAL (see FMeshletDraw): the view picked the LOD, so the entry has to name the
// meshlet outright rather than an offset into a range the reader would have to guess.
void EmitMeshlet(FCullView View, uint DrawID, uint InstanceID, uint MeshletIdx)
{
    // A DrawID past the view's slice would make every atomic below land outside the args allocation.
    // The instance's DrawID is stamped by the game thread and the arg arrays are sized from the batch
    // count, so this should be unreachable -- but an out-of-bounds atomic corrupts unrelated GPU memory
    // and surfaces as an unrelated shader faulting, which is not a thing to leave to an invariant.
    if (DrawID >= View.NumDraws)
    {
        return;
    }

    uint IndirectIndex = View.IndirectArgsOffset + DrawID;
    uint OldCount;
    InterlockedAdd(Pass().IndirectArgs[IndirectIndex].InstanceCount, 1, OldCount);

    // Same survivor count, written straight into the mesh-task GroupCountX so ConvertMeshDrawArgs is gone.
    // Must be atomic (each lane only knows its own +1, not the total); HW coalesces same-address-in-wave.
    uint MeshOld;
    InterlockedAdd(Pass().MeshDrawArgs[IndirectIndex].GroupCountX, 1u, MeshOld);

    // This (view, draw)'s region is sized to exactly what it can emit, so OldCount alone bounds it: in a
    // frame whose total fits the allocation this can never reject. It only fires when the allocation itself
    // came up short, and BuildDrawPrefix already collapsed the regions that could not fit to zero length.
    // The absolute bound stays as the outer backstop against a failed allocation.
    uint Slot = Pass().ViewDrawOffsets[IndirectIndex] + OldCount;
    if (OldCount >= Pass().ViewDrawCounts[IndirectIndex] || Slot >= Pass().DrawListCapacity)
    {
        return;   // allocation came up short this frame; the next one grows from OutTotals[2]
    }

    FMeshletDraw Draw;
    Draw.InstanceID        = InstanceID;
    Draw.PackedIndexAndTag = FMeshletDraw::Pack(MeshletIdx, Scene().CullData.MeshletDrawTag);
    MeshletDrawList()[Slot] = Draw;
}

// Recover (InstanceID, MeshletWalkIdx) from a flat thread index via binary search on the inclusive
// per-instance prefix sum: prefix[InstanceID] <= flat_idx < prefix[InstanceID + 1]. O(log NumInstances).
bool ResolveInstanceFromFlatIndex(uint flat_idx, out uint InstanceID, out uint MeshletWalkIdx)
{
    InstanceID     = 0u;
    MeshletWalkIdx = 0u;

    uint TotalWork = Pass().Totals[0];
    if (flat_idx >= TotalWork)
    {
        return false;
    }

    // Find the largest i with InstanceMeshletPrefix()[i] <= flat_idx. Invariant: prefix[lo] <= flat_idx
    // < prefix[hi] (hi may be the sentinel NumInstances); prefix[0] == 0 keeps the lo=0 base case valid.
    uint Lo = 0u;
    uint Hi = Pass().Totals[1];
    while (Hi - Lo > 1u)
    {
        uint Mid = (Lo + Hi) >> 1u;
        if (InstanceMeshletPrefix()[Mid] <= flat_idx)
        {
            Lo = Mid;
        }
        else
        {
            Hi = Mid;
        }
    }

    InstanceID     = Lo;
    MeshletWalkIdx = flat_idx - InstanceMeshletPrefix()[Lo];
    return true;
}

// Phase 0: walks every (instance, meshlet) pair, routing survivors into a view's draw list or the
// defer list (camera + HZB-occluded). Flat 1D dispatch, one thread per meshlet; the 65535 Y fold
// from the C++ side is reconstructed into a flat index here.
void RunEarlyPhase(uint3 GlobalID, uint3 GroupID, uint NumViews)
{
    const uint flat_idx = GlobalID.x + GroupID.y * 65535u * 64u;

    uint InstanceID;
    uint MeshletWalkIdx;
    if (!ResolveInstanceFromFlatIndex(flat_idx, InstanceID, MeshletWalkIdx))
    {
        return;
    }

    FGPUInstance Inst = GetInstance(InstanceID);

    // The walk domain covers the WIDEST range any view will ask for: each non-shadow view picks its own
    // LOD, and a shadow view walks the shadow LOD (capped at MESHLET_MAX_SHADOW_LOD, so it can be the
    // finer -- and larger -- of the two on distant casters). CullInstances computed that max while it
    // reserved space; a thread past it has no meshlet in any view.
    //
    // This value is computed in ONE place now (CullInstances, into MeshletWalkCount) and READ here, by
    // ScanPrefixBlocks for the prefix that indexes this domain, and by the CPU's skinned seed. It used to
    // be re-derived at each of those sites, which is three chances to disagree and drop the tail.
    if (MeshletWalkIdx >= Inst.MeshletWalkCount)
    {
        return;
    }

    // A null header with a non-zero meshlet count should be impossible -- the gather forces the count to
    // zero whenever the address is null -- but it is only an invariant, and violating it dereferences
    // address 0 below and loses the device. Cheap enough to enforce here rather than trust it.
    if (Inst.MeshletHeader == nullptr)
    {
        return;
    }

    // Per-view LOD table. Loaded once per thread (the descs are interned, so this is a handful of distinct
    // lines for the whole scene); the pick itself is a dot product and a short compare chain per view.
    // kNoSurfaceDesc means the instance's LOD is fixed -- skinned, forced, or LODs off -- and every view
    // takes the range as it stands.
    const bool bPerViewLOD = Inst.SurfaceDescIndex != kNoSurfaceDesc
                          && Inst.SurfaceDescIndex < Pass().NumSurfaceDescs;
    FSurfaceDesc Desc = (FSurfaceDesc)0;
    if (bPerViewLOD)
    {
        Desc = Pass().SurfaceDescs[Inst.SurfaceDescIndex];
    }

    // Per-instance / per-meshlet data reused across every view.
    bool bSkinned  = Inst.HasFlag(EInstanceFlags::Skinned);
    bool bNoCull   = Inst.HasFlag(EInstanceFlags::NoCull);
    bool bCaster   = Inst.HasFlag(EInstanceFlags::CastShadow);
    bool bTwoSided = Inst.HasFlag(EInstanceFlags::TwoSided);

    float4x4 Model = Inst.ModelMatrix;

    float SqScaleX = dot(Model[0].xyz, Model[0].xyz);
    float SqScaleY = dot(Model[1].xyz, Model[1].xyz);
    float SqScaleZ = dot(Model[2].xyz, Model[2].xyz);
    float MaxScale = sqrt(max(SqScaleX, max(SqScaleY, SqScaleZ)));

    // Bounds for the meshlet the CURRENT view resolved to. Nothing can be hoisted out of the loop any
    // more: two views at different LODs address different primitives at the same walk index. Cached on the
    // resolved index instead, so the common cases -- four cascades sharing the shadow LOD, a capture that
    // happens to agree with the primary -- still load once.
    uint           CachedMeshletIdx = ~0u;
    FMeshletBounds Bounds           = (FMeshletBounds)0;
    float3         WorldCenter      = float3(0.0, 0.0, 0.0);
    float          WorldRadius      = 0.0;
    float3         WorldApex        = float3(0.0, 0.0, 0.0);
    float3         WorldAxis        = float3(0.0, 0.0, 1.0);
    bool           bHasCone         = false;

    uint DrawID = Inst.DrawID;

    // Instance-level camera distance reused for CastShadowOnly views.
    float3 CamPos        = GetCameraPosition();
    float3 InstCenter    = Inst.BoundsCenter;
    float  InstRadius    = Inst.BoundsRadius;
    float  DistToCamera  = length(InstCenter - CamPos);
    float  MaxShadowDist = Scene().CullData.ShadowMaxDistance;

    for (uint v = 0; v < NumViews; ++v)
    {
        FCullView View = CullViews()[v];

        // Camera-late view is owned by phase 1; skip in phase 0 so we don't
        // emit doubles when a meshlet passes both HZB tests.
        if (View.HasFlag(CULL_VIEW_FLAG_PHASE_LATE))
            continue;

        if (View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY) && !bCaster)
            continue;

        if (View.HasFlag(CULL_VIEW_FLAG_DISTANCE))
        {
            if (DistToCamera - InstRadius > MaxShadowDist)
                continue;
        }

        // Which meshlet range this view rasters. A shadow view takes the pre-made shadow pick (the cascades
        // are fit to the primary camera, so their LOD is the primary's distance, biased); every other view
        // picks its own from its own origin -- which is the whole point of this pass being per view.
        const bool bShadowView = View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY);

        uint RangeOffset;
        uint RangeCount;
        if (bShadowView)
        {
            RangeOffset = Inst.ShadowMeshletOffset;
            RangeCount  = Inst.ShadowMeshletCount;
        }
        else if (bPerViewLOD)
        {
            // Byte-for-byte the pick CullInstances reserved this view's draw-list region from.
            const uint2 Range = SelectViewMeshletRange(Desc, View.ViewOrigin, Inst.SphereBounds);
            RangeOffset = Range.x;
            RangeCount  = Range.y;
        }
        else
        {
            RangeOffset = Inst.SurfaceMeshletOffset;
            RangeCount  = Inst.SurfaceMeshletCount;
        }

        // The walk domain is the max over views, so a thread past THIS view's count exists only to cover
        // some other view's finer LOD and has no meshlet here.
        if (MeshletWalkIdx >= RangeCount)
            continue;

        const uint MeshletIdx = RangeOffset + MeshletWalkIdx;

        // Mesh-global bound, the same one the late phase and MeshletGeometry already apply. RangeCount
        // only says how far into the RANGE this thread sits; nothing above proves RangeOffset + it lands
        // inside the header's arrays, and a bad range walks Bounds[] past the allocation. Those are
        // scattered 48-byte BDA reads, which return garbage rather than faulting -- so the symptom is a
        // multi-second hang and a TDR with a live wave parked on the load below and NO page fault.
        if (MeshletIdx >= Inst.MeshletTotalCount)
            continue;

        if (MeshletIdx != CachedMeshletIdx)
        {
            CachedMeshletIdx = MeshletIdx;
            Bounds      = Inst.MeshletHeader.Bounds[MeshletIdx];
            WorldCenter = mul(Model, float4(Bounds.Center, 1.0)).xyz;
            WorldRadius = Bounds.Radius * MaxScale;
            WorldApex   = mul(Model, float4(Bounds.ConeApex, 1.0)).xyz;
            WorldAxis   = normalize(mul((float3x3)Model, Bounds.ConeAxis));
            bHasCone    = Bounds.ConeCutoff < 1.0;
        }

        const FMeshletBounds VBounds      = Bounds;
        const float3         VWorldCenter = WorldCenter;
        const float          VWorldRadius = WorldRadius;
        const float3         VWorldApex   = WorldApex;
        const float3         VWorldAxis   = WorldAxis;
        const bool           VHasCone     = bHasCone;

        // Cone cull (skipped for skinned/two-sided/NoCull). Sun views test
        // against sun direction; camera/point/spot against apex-to-origin.
        if (!bSkinned && !bNoCull && !bTwoSided && VHasCone && View.HasFlag(CULL_VIEW_FLAG_CONE))
        {
            if (View.HasFlag(CULL_VIEW_FLAG_SUN_ALIGNED))
            {
                float3 SunDir = GetSunDirection();
                if (dot(SunDir, VWorldAxis) <= -VBounds.ConeCutoff)
                {
                    continue;
                }
            }
            else
            {
                float3 ApexToOrigin    = VWorldApex - View.ViewOrigin;
                float  ApexToOriginLen = length(ApexToOrigin);
                if (dot(ApexToOrigin, VWorldAxis) >= VBounds.ConeCutoff * ApexToOriginLen)
                    continue;
            }
        }

        if (!bSkinned && !bNoCull && View.HasFlag(CULL_VIEW_FLAG_FRUSTUM))
        {
            if (!FrustumContainsSphere(View.FrustumPlanes, VWorldCenter, VWorldRadius))
                continue;
        }

        // Cascade-specific rejects. The frustum test above is against the cascade's ortho box, which is
        // fitted to a sphere around the camera sub-frustum slice -- and those spheres nest, so the box alone
        // barely separates one cascade from the next. Everything that actually makes a cascade cheap is here.
        if (!bNoCull && View.HasFlag(CULL_VIEW_FLAG_CASCADE))
        {
            const uint CascadeIndex = View.CascadeIndex;

            // The slice this cascade shades, swept toward the sun: the exact caster set, and unlike the
            // ortho box it does not nest. This is what stops cascade 3 from re-rastering cascade 0's scene.
            if (!FrustumContainsSphere(Scene().CullData.CascadeFrustum[CascadeIndex], VWorldCenter, VWorldRadius))
                continue;

            // Micro-poly reject. MinBoundsDiameter is a world length derived from the cascade's own texel
            // pitch, so the same meshlet survives in cascade 0 and dies in cascade 3 -- which is the point:
            // the coarse cascades are where sub-texel geometry costs the most and shows the least. What it
            // leaves behind is by construction narrower than one shadow texel.
            if (View.MinBoundsDiameter > 0.0 && (VWorldRadius * 2.0) < View.MinBoundsDiameter)
                continue;

            // Hi-Z against LAST frame's cascade depth. Skinned meshes are excluded for the same reason they
            // are on the camera path: their bounds are re-fed each frame and the pyramid describes the pose
            // that is already gone.
            if (!bSkinned && Scene().CullData.bShadowOcclusionCull != 0u &&
                IsCascadeHZBOccluded(CascadeIndex, VWorldCenter, VWorldRadius))
            {
                continue;
            }
        }

        // Hi-Z occlusion runs only on the camera view (the pyramid tracks the camera); an occluded
        // meshlet is deferred to phase 1 rather than dropped.
        if (!bSkinned && !bNoCull && View.HasFlag(CULL_VIEW_FLAG_OCCLUSION))
        {
            // Micro-poly reject: if the meshlet projects sub-pixel we can drop
            // it entirely. Packaged with occlusion so shadow views don't pay.
            float3 ViewCenter = mul(Scene().CameraView.CameraView, float4(WorldCenter, 1.0)).xyz;
            float4 ClipAABB;
            if (ProjectSphereClip(ViewCenter, WorldRadius,
                                  Scene().NearPlane,
                                  Scene().CameraView.CameraProjection[0][0],
                                  Scene().CameraView.CameraProjection[1][1],
                                  ClipAABB))
            {
                float2 ScreenSize = float2(Scene().ScreenSize.xy);
                float2 MinPx = (ClipAABB.xy * 0.5 + 0.5) * ScreenSize;
                float2 MaxPx = (ClipAABB.zw * 0.5 + 0.5) * ScreenSize;

                int2 MinI = int2(floor(MinPx - 0.5));
                int2 MaxI = int2(floor(MaxPx - 0.5));

                if (MinI.x == MaxI.x || MinI.y == MaxI.y)
                    continue;
            }

            if (IsHiZOccluded(WorldCenter, WorldRadius))
            {
                // Defer to phase 1. Phase 1 owns the camera-late view slice
                // and re-tests against the rebuilt HZB before emitting.
                uint DeferSlot;
                InterlockedAdd(Pass().DeferCount[0], 1, DeferSlot);

                // The counter is unbounded on purpose (it doubles as the demand measurement the CPU sizes
                // the next allocation from), so the WRITE has to be bounded here. Dropping a deferred
                // meshlet costs one frame of it not being re-tested against the rebuilt HZB -- and it was
                // HZB-occluded, so it was most likely invisible anyway.
                if (DeferSlot < Pass().DeferListCapacity)
                {
                    FMeshletDeferred Entry;
                    Entry.InstanceID   = InstanceID;
                    Entry.MeshletIndex = MeshletIdx;
                    Entry.DrawID       = DrawID;
                    Entry._Pad         = 0u;
                    Pass().DeferList[DeferSlot] = Entry;
                }
                continue;
            }
        }

        EmitMeshlet(View, DrawID, InstanceID, MeshletIdx);
    }
}

// Phase 1: one thread per deferred entry, no scene walk; every survivor goes to the camera-late view.
void RunLatePhase(uint3 GlobalID, uint3 GroupID, uint CameraLateViewIndex)
{
    // Same flat-index reconstruction as RunEarlyPhase (large dispatches folded into Y on the C++ side).
    uint ThreadIdx = GlobalID.x + GroupID.y * 65535u * 64u;
    // Clamped: only the first DeferListCapacity entries were actually written on an overflow frame.
    uint Count     = min(Pass().DeferCount[0], Pass().DeferListCapacity);

    if (ThreadIdx >= Count)
        return;

    FMeshletDeferred Entry = Pass().DeferList[ThreadIdx];

    // Reconstruct world-space bounds as phase 0 did; transforms are baked into the Instance buffer
    // before phase 0, so re-deriving is stable.
    FGPUInstance Inst = GetInstance(Entry.InstanceID);

    // The entry names the meshlet outright, so the late phase no longer has to know which LOD the early
    // phase picked -- it bounds against the mesh's whole meshlet array instead of re-deriving a range.
    // (The late view is the same camera as the early one, so it would agree either way; not needing to
    // agree is what makes this correct for a view whose LOD is its own.)
    if (Entry.MeshletIndex >= Inst.MeshletTotalCount || Inst.MeshletHeader == nullptr)
    {
        return;
    }

    FMeshletBounds Bounds = Inst.MeshletHeader.Bounds[Entry.MeshletIndex];

    float4x4 Model = Inst.ModelMatrix;
    float3 WorldCenter = mul(Model, float4(Bounds.Center, 1.0)).xyz;

    float SqScaleX = dot(Model[0].xyz, Model[0].xyz);
    float SqScaleY = dot(Model[1].xyz, Model[1].xyz);
    float SqScaleZ = dot(Model[2].xyz, Model[2].xyz);
    float MaxScale = sqrt(max(SqScaleX, max(SqScaleY, SqScaleZ)));
    float WorldRadius = Bounds.Radius * MaxScale;

    // Re-test against the rebuilt HZB: still-occluded meshlets are genuinely hidden and dropped;
    // ones that now pass were hidden by a stale pyramid. Micro-poly reject already fired in phase 0.
    if (IsHiZOccluded(WorldCenter, WorldRadius))
        return;

    FCullView CameraLate = CullViews()[CameraLateViewIndex];
    EmitMeshlet(CameraLate, Entry.DrawID, Entry.InstanceID, Entry.MeshletIndex);
}

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(uint3 GlobalID : SV_DispatchThreadID, uint3 GroupID : SV_GroupID)
{
    if (Pass().Phase == CULL_PHASE_LATE)
    {
        RunLatePhase(GlobalID, GroupID, Pass().CameraLateViewIndex);
    }
    else
    {
        RunEarlyPhase(GlobalID, GroupID, Pass().NumViews);
    }
}
