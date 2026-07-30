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
    uint _Pad;
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
    // Entries the shared meshlet draw list can hold; outer backstop for a short allocation.
    uint                             DrawListCapacity;
    // Entries the defer list can hold. DeferCount is an unbounded atomic, so this is what stops the
    // append from writing past the allocation -- and what stops the late phase reading past it.
    uint                             DeferListCapacity;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Appends a surviving meshlet into the view's slice; centralized so both phases share the atomic protocol.
void EmitMeshlet(FCullView View, uint DrawID, uint InstanceID, uint MeshletLocalIdx)
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
    Draw.PackedIndexAndTag = FMeshletDraw::Pack(MeshletLocalIdx, Scene().CullData.MeshletDrawTag);
    MeshletDrawList()[Slot] = Draw;
}

// Recover (InstanceID, MeshletLocalIdx) from a flat thread index via binary search on the inclusive
// per-instance prefix sum: prefix[InstanceID] <= flat_idx < prefix[InstanceID + 1]. O(log NumInstances).
bool ResolveInstanceFromFlatIndex(uint flat_idx, out uint InstanceID, out uint MeshletLocalIdx)
{
    InstanceID      = 0u;
    MeshletLocalIdx = 0u;

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

    InstanceID      = Lo;
    MeshletLocalIdx = flat_idx - InstanceMeshletPrefix()[Lo];
    return true;
}

// Phase 0: walks every (instance, meshlet) pair, routing survivors into a view's draw list or the
// defer list (camera + HZB-occluded). Flat 1D dispatch, one thread per meshlet; the 65535 Y fold
// from the C++ side is reconstructed into a flat index here.
void RunEarlyPhase(uint3 GlobalID, uint3 GroupID, uint NumViews)
{
    const uint flat_idx = GlobalID.x + GroupID.y * 65535u * 64u;

    uint InstanceID;
    uint MeshletLocalIdx;
    if (!ResolveInstanceFromFlatIndex(flat_idx, InstanceID, MeshletLocalIdx))
    {
        return;
    }

    FGPUInstance Inst = GetInstance(InstanceID);

    // The domain covers BOTH LODs: a shadow-only view walks the SHADOW LOD, which is capped at
    // MESHLET_MAX_SHADOW_LOD and so is the FINER (more meshlets) of the two once the camera LOD passes the
    // cap. Sizing it on the surface count alone left [SurfaceCount, ShadowCount) with no thread at all.
    //
    // This value is computed in FOUR places -- here, ScanPrefixBlocks (the prefix), CullInstances
    // (OutBatchMeshletCount, the dispatch size) and the CPU's skinned seed. Any two disagreeing drops
    // meshlets off the tail of the domain.
    const uint SurfaceCount = Inst.SurfaceMeshletCount;
    const uint ShadowCount  = Inst.ShadowMeshletCount;
    if (MeshletLocalIdx >= max(SurfaceCount, ShadowCount))
    {
        return;
    }

    // Past the surface count this thread exists only for the shadow LOD: the same local index addresses a
    // different LOD's meshlet, so nothing may read the surface bounds for it.
    const bool bHasSurface = MeshletLocalIdx < SurfaceCount;

    // A null header with a non-zero meshlet count should be impossible -- the gather forces the count to
    // zero whenever the address is null -- but it is only an invariant, and violating it dereferences
    // address 0 below and loses the device. Cheap enough to enforce here rather than trust it.
    if (Inst.MeshletHeader == nullptr)
    {
        return;
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

    // Camera-LOD bounds for the camera and non-shadow views. Shadow-only views fetch their own
    // bounds from the shadow LOD in the per-view loop (different primitive at the same local index).
    // Guarded: a shadow-only thread's index is past the surface LOD's meshlet range, and because LODs are
    // packed back to back this would read the NEXT LOD's bounds rather than fault -- a wrong cull decision
    // that looks like nothing at all went wrong.
    FMeshletBounds Bounds      = (FMeshletBounds)0;
    float3         WorldCenter = float3(0.0, 0.0, 0.0);
    float          WorldRadius = 0.0;
    float3         WorldApex   = float3(0.0, 0.0, 0.0);
    float3         WorldAxis   = float3(0.0, 0.0, 1.0);
    bool           bHasCone    = false;

    if (bHasSurface)
    {
        Bounds      = Inst.MeshletHeader.Bounds[Inst.SurfaceMeshletOffset + MeshletLocalIdx];
        WorldCenter = mul(Model, float4(Bounds.Center, 1.0)).xyz;
        WorldRadius = Bounds.Radius * MaxScale;
        WorldApex   = mul(Model, float4(Bounds.ConeApex, 1.0)).xyz;
        WorldAxis   = normalize(mul((float3x3)Model, Bounds.ConeAxis));
        bHasCone    = Bounds.ConeCutoff < 1.0;
    }

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

        // Shadow-only views test the shadow LOD's meshlet (its own Center/Radius/Cone); threads
        // past the shadow LOD's per-instance count have no meshlet here and skip the view.
        bool bShadowView = View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY);

        // Every non-shadow view rasters the surface LOD, so a thread that exists only to cover the shadow
        // LOD's extra meshlets has nothing to contribute here.
        if (!bShadowView && !bHasSurface)
            continue;

        FMeshletBounds VBounds      = Bounds;
        float3         VWorldCenter = WorldCenter;
        float          VWorldRadius = WorldRadius;
        float3         VWorldApex   = WorldApex;
        float3         VWorldAxis   = WorldAxis;
        bool           VHasCone     = bHasCone;

        if (bShadowView)
        {
            if (MeshletLocalIdx >= Inst.ShadowMeshletCount)
                continue;

            uint ShadowGlobalIdx = Inst.ShadowMeshletOffset + MeshletLocalIdx;
            VBounds      = Inst.MeshletHeader.Bounds[ShadowGlobalIdx];
            VWorldCenter = mul(Model, float4(VBounds.Center, 1.0)).xyz;
            VWorldRadius = VBounds.Radius * MaxScale;
            VWorldApex   = mul(Model, float4(VBounds.ConeApex, 1.0)).xyz;
            VWorldAxis   = normalize(mul((float3x3)Model, VBounds.ConeAxis));
            VHasCone     = VBounds.ConeCutoff < 1.0;
        }

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
                    Entry.InstanceID        = InstanceID;
                    Entry.MeshletLocalIndex = MeshletLocalIdx;
                    Entry.DrawID            = DrawID;
                    Entry._Pad              = 0u;
                    Pass().DeferList[DeferSlot] = Entry;
                }
                continue;
            }
        }

        EmitMeshlet(View, DrawID, InstanceID, MeshletLocalIdx);
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
    if (Entry.MeshletLocalIndex >= Inst.SurfaceMeshletCount)
    {
   		return;
    }

    uint GlobalMeshletIdx = Inst.SurfaceMeshletOffset + Entry.MeshletLocalIndex;
    FMeshletBounds Bounds = Inst.MeshletHeader.Bounds[GlobalMeshletIdx];

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
    EmitMeshlet(CameraLate, Entry.DrawID, Entry.InstanceID, Entry.MeshletLocalIndex);
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
