#pragma once
#include "Culling.slang"

// The per-meshlet cull body, shared by every geometry pass's task (amplification) stage, which is the
// identical tests. Two rules govern everything here, and both exist because this code is about to be
// compiled into a stage it was not written for.
//
// RULE 1 -- NO EARLY RETURNS, NO `continue`. Every reject sets a flag and falls through.
//   The compute cull could reject with `continue` because a thread that leaves the view loop early
//   affects nothing else. A task shader cannot: it ends in a group barrier and a uniform DispatchMesh, and
//   a lane that returned before the barrier makes that barrier non-uniform. That is undefined behaviour
//   which NVIDIA happens to tolerate and AMD hangs on -- a TDR with no page fault and no failing lane to
//   inspect. Structuring the rejects as nested predicates instead costs nothing (the compiler flattens
//   them the same way) and makes the function safe to inline under a barrier.
//
// RULE 2 -- NO Pass() ACCESS. This file touches no push constants at all: it takes what it needs as
//   arguments and RETURNS a decision, leaving the caller to perform the emit or the defer against
//   whatever buffers its own push block names. The compute cull and the task cull have different push
//   layouts; a shared body that reached for Pass() would be shared in name only.
//
// The LOD range selection is NOT here: BuildMeshletBlocks resolved it when it built the block, so this
// takes a mesh-global meshlet index and makes no LOD decision at all.

// ----------------------------------------------------------------------------------------------------
// Per-(instance, walk index) state, built once and reused across every view the caller tests.
// ----------------------------------------------------------------------------------------------------
struct FMeshletCullContext
{
    uint     DrawID;

    // Instance flags, unpacked once rather than re-tested per view.
    bool     bSkinned;
    bool     bNoCull;
    bool     bCaster;
    bool     bTwoSided;
    float4x4 Model;
    float    MaxScale;

    // Instance sphere + camera distance, reused by the distance reject and by the cascade path (which
    // tests the instance sphere rather than the bind-pose meshlet sphere for skinned meshes).
    float3   InstCenter;
    float    InstRadius;
    float    DistToCamera;
    float    MaxShadowDist;

};

// NOTE ON THE `FGPUInstance*` PARAMETERS BELOW
//
// Taken by POINTER, never by value. FGPUInstance carries a FMeshletHeader* (PhysicalStorageBuffer), and a
// by-value parameter makes Slang materialize that pointer into a Function OpVariable -- which SPIR-V
// requires to carry Aliased or Restrict, and which Slang emits with neither. The module is invalid, every
// driver accepts it anyway, and the compiler is then free to reorder loads through that pointer against
// stores it assumes cannot alias. It surfaces frames later as VK_ERROR_DEVICE_LOST with no shader
// attribution. Slang auto-derefs field access, so `Inst.Field` reads identically either way.
FMeshletCullContext MakeMeshletCullContext(FGPUInstance* Inst)
{
    FMeshletCullContext Ctx;
    Ctx.DrawID = Inst.DrawID;

    Ctx.bSkinned    = Inst.HasFlag(EInstanceFlags::Skinned);
    Ctx.bNoCull     = Inst.HasFlag(EInstanceFlags::NoCull);
    Ctx.bCaster     = Inst.HasFlag(EInstanceFlags::CastShadow);
    Ctx.bTwoSided   = Inst.HasFlag(EInstanceFlags::TwoSided);

    Ctx.Model = Inst.ModelMatrix;

    const float SqScaleX = dot(Ctx.Model[0].xyz, Ctx.Model[0].xyz);
    const float SqScaleY = dot(Ctx.Model[1].xyz, Ctx.Model[1].xyz);
    const float SqScaleZ = dot(Ctx.Model[2].xyz, Ctx.Model[2].xyz);
    Ctx.MaxScale = sqrt(max(SqScaleX, max(SqScaleY, SqScaleZ)));

    Ctx.InstCenter    = Inst.BoundsCenter;
    Ctx.InstRadius    = Inst.BoundsRadius;
    Ctx.DistToCamera  = length(Ctx.InstCenter - GetCameraPosition());
    Ctx.MaxShadowDist = Scene().CullData.ShadowMaxDistance;

    return Ctx;
}

// ----------------------------------------------------------------------------------------------------
// World-space bounds of the meshlet the CURRENT view resolved to.
//
// Nothing here can be hoisted out of a view loop: two views at different LODs address different meshlets
// at the same walk index. Cached on the RESOLVED index instead, so the common cases -- four cascades
// sharing a shadow LOD, a capture that agrees with the primary view -- still load once. One entry deep,
// which is all the ordering of BuildCullViews makes useful.
// ----------------------------------------------------------------------------------------------------
struct FMeshletBoundsCache
{
    uint           MeshletIdx;
    float          ConeCutoff;
    float3         WorldCenter;
    float          WorldRadius;
    float3         WorldApex;
    float3         WorldAxis;
    bool           bHasCone;
    bool           bConeReady;
};

FMeshletBoundsCache MakeMeshletBoundsCache()
{
    FMeshletBoundsCache C;
    C.MeshletIdx  = ~0u;
    C.ConeCutoff  = 1.0;
    C.WorldCenter = float3(0.0, 0.0, 0.0);
    C.WorldRadius = 0.0;
    C.WorldApex   = float3(0.0, 0.0, 0.0);
    C.WorldAxis   = float3(0.0, 0.0, 1.0);
    C.bHasCone    = false;
    C.bConeReady  = false;
    return C;
}

void RefreshMeshletBounds(FGPUInstance* Inst, float4x4 Model, float MaxScale, uint MeshletIdx, inout FMeshletBoundsCache C)
{
    if (MeshletIdx != C.MeshletIdx)
    {
        const FMeshletSphere S = Inst.MeshletHeader.Spheres[MeshletIdx];

        C.MeshletIdx  = MeshletIdx;
        C.WorldCenter = mul(Model, float4(S.Center, 1.0)).xyz;
        C.WorldRadius = S.Radius * MaxScale;
        C.bConeReady  = false;
        C.bHasCone    = true;   // resolved with the cone itself; the sphere carries no cone bit
    }
}

// The cone, loaded and transformed only when a view actually tests it. Kept out of the sphere because
// every meshlet of every view pays that load, while this one needs a non-skinned, non-two-sided,
// cone-bearing meshlet in a cone-enabled view -- and the late re-test never touches it at all.
void EnsureMeshletCone(FGPUInstance* Inst, float4x4 Model, inout FMeshletBoundsCache C)
{
    if (!C.bConeReady)
    {
        const FMeshletCone Cone = Inst.MeshletHeader.Cones[C.MeshletIdx];

        C.WorldApex   = mul(Model, float4(Cone.Apex, 1.0)).xyz;
        C.WorldAxis   = normalize(mul((float3x3)Model, Cone.Axis));
        C.ConeCutoff  = Cone.Cutoff;
        C.bHasCone    = Cone.Cutoff < 1.0;
        C.bConeReady  = true;
    }
}

// ----------------------------------------------------------------------------------------------------
// The decision. Exactly one of bEmit / bDefer can be true; both false means rejected outright.
// ----------------------------------------------------------------------------------------------------
struct FMeshletCullResult
{
    bool bEmit;
    bool bDefer;
    // Which end of the defer arena this belongs in, and -- for a cascade -- which pyramid re-tests it.
    // Both only meaningful when bDefer.
    bool bDeferCascade;
    uint CascadeIndex;
    uint MeshletIdx;  // mesh-global; only meaningful when bEmit or bDefer
};

// One meshlet, one view. Nested predicates rather than early exits -- see RULE 1 at the top of the file.
FMeshletCullResult TestMeshletAgainstView(FMeshletCullContext Ctx, FGPUInstance* Inst, FCullView View,
                                          uint MeshletIdx, inout FMeshletBoundsCache Cache)
{
    FMeshletCullResult R;
    R.bEmit         = false;
    R.bDefer        = false;
    R.bDeferCascade = false;
    R.CascadeIndex  = 0u;
    R.MeshletIdx    = 0u;

    // Views this call does not own, and instance-level rejects that need no meshlet at all.
    //  - PHASE_LATE is owned by the late phase; testing it here would emit a meshlet twice when it passes
    //    both HZB tests.
    //  - CAST_SHADOW_ONLY views only want casters.
    //  - DISTANCE views drop instances past the shadow distance, measured off the instance sphere.
    bool bKeep = !View.HasFlag(CULL_VIEW_FLAG_PHASE_LATE)
              && !(View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY) && !Ctx.bCaster)
              && !(View.HasFlag(CULL_VIEW_FLAG_DISTANCE) && (Ctx.DistToCamera - Ctx.InstRadius > Ctx.MaxShadowDist));

    if (bKeep)
    {
        // Mesh-global bound. The block already resolved the view's range and clamped its length, so this
        // should be unreachable -- it stays because an unbounded index walks Bounds[] past the allocation,
        // and those are scattered 48-byte BDA reads that return garbage rather than faulting: a
        // multi-second hang and a TDR with a live wave parked on the load and no page fault to point at.
        bKeep = MeshletIdx < Inst.MeshletTotalCount;

        if (bKeep)
        {
            R.MeshletIdx = MeshletIdx;
            RefreshMeshletBounds(Inst, Ctx.Model, Ctx.MaxScale, MeshletIdx, Cache);

            // Cone cull (skipped for skinned / two-sided / NoCull). Sun views test against the sun
            // direction; camera, point and spot against apex-to-origin.
            if (!Ctx.bSkinned && !Ctx.bNoCull && !Ctx.bTwoSided && View.HasFlag(CULL_VIEW_FLAG_CONE))
            {
                EnsureMeshletCone(Inst, Ctx.Model, Cache);

                // Cutoff 1.0 means the build found no usable cone, so the bit now arrives with the cone
                // rather than gating the load that produces it.
                if (Cache.bHasCone)
                {
                    if (View.HasFlag(CULL_VIEW_FLAG_SUN_ALIGNED))
                    {
                        bKeep = dot(GetSunDirection(), Cache.WorldAxis) > -Cache.ConeCutoff;
                    }
                    else
                    {
                        const float3 ApexToOrigin    = Cache.WorldApex - View.ViewOrigin;
                        const float  ApexToOriginLen = length(ApexToOrigin);
                        bKeep = dot(ApexToOrigin, Cache.WorldAxis) < Cache.ConeCutoff * ApexToOriginLen;
                    }
                }
            }

            if (bKeep && !Ctx.bSkinned && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_FRUSTUM))
            {
                bKeep = FrustumContainsSphere(View.FrustumPlanes, Cache.WorldCenter, Cache.WorldRadius);
            }

            // Cascade-specific rejects. The frustum test above is against the cascade's ortho box, which is
            // fitted to a sphere around the camera sub-frustum slice -- and those spheres nest, so the box
            // alone barely separates one cascade from the next. Everything that makes a cascade cheap is here.
            if (bKeep && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_CASCADE))
            {
                const uint CascadeIndex = View.CascadeIndex;

                // A skinned mesh's MESHLET bounds are bind-pose and say nothing about where the meshlet
                // actually is once skinned, which is why every other cull here excludes skinned outright.
                // The INSTANCE sphere is re-fed each frame and does track the pose, so cascades test that
                // instead: coarser than the meshlet sphere, but a true bound rather than a stale one.
                // Testing the meshlet sphere here punched pose-dependent holes in character shadows.
                const float3 CullCenter = Ctx.bSkinned ? Ctx.InstCenter : Cache.WorldCenter;
                const float  CullRadius = Ctx.bSkinned ? Ctx.InstRadius : Cache.WorldRadius;

                // The slice this cascade shades, swept toward the sun: the exact caster set, and unlike the
                // ortho box it does not nest. This is what stops cascade 3 re-rastering cascade 0's scene.
                bKeep = FrustumContainsSphere(Scene().CullData.CascadeFrustum[CascadeIndex], CullCenter, CullRadius);

                // Micro-poly reject. MinBoundsDiameter is a world length derived from the cascade's own
                // texel pitch, so the same meshlet survives in cascade 0 and dies in cascade 3 -- which is
                // the point: the coarse cascades are where sub-texel geometry costs most and shows least.
                //
                // Skinned is excluded rather than switched to the instance sphere: this rejects on
                // SMALLNESS, and the instance sphere is the whole character, so it would never reject
                // anything. A bind-pose meshlet radius is not a safe stand-in for a size test.
                if (bKeep && !Ctx.bSkinned && View.MinBoundsDiameter > 0.0)
                {
                    bKeep = (Cache.WorldRadius * 2.0) >= View.MinBoundsDiameter;
                }

                // Hi-Z against LAST frame's cascade depth, DEFERRED rather than dropped exactly as the
                // camera path does it. This pyramid is a frame old, so a caster hidden last frame and
                // exposed this one would otherwise go missing for a frame -- a shadow that flickers open
                // under motion. The late cascade phase re-tests against the pyramid rebuilt from this
                // frame's own raster.
                if (bKeep && !Ctx.bSkinned && Scene().CullData.bShadowOcclusionCull != 0u &&
                    IsCascadeHZBOccluded(CascadeIndex, Cache.WorldCenter, Cache.WorldRadius))
                {
                    R.bDefer        = true;
                    R.bDeferCascade = true;
                    R.CascadeIndex  = CascadeIndex;
                    bKeep = false;
                }
            }

            // Hi-Z occlusion runs only on the camera view (the pyramid tracks the camera); an occluded
            // meshlet is deferred to the late phase rather than dropped.
            if (bKeep && !Ctx.bSkinned && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_OCCLUSION))
            {
                // Micro-poly reject: a meshlet that projects sub-pixel is dropped outright. Packaged with
                // occlusion so shadow views do not pay for it.
                float4 ClipAABB;
                if (ProjectSphereClip(mul(Scene().CameraView.CameraView, float4(Cache.WorldCenter, 1.0)).xyz,
                                      Cache.WorldRadius,
                                      Scene().NearPlane,
                                      Scene().CameraView.CameraProjection[0][0],
                                      Scene().CameraView.CameraProjection[1][1],
                                      ClipAABB))
                {
                    const float2 ScreenSize = float2(Scene().ScreenSize.xy);
                    const int2   MinI = int2(floor((ClipAABB.xy * 0.5 + 0.5) * ScreenSize - 0.5));
                    const int2   MaxI = int2(floor((ClipAABB.zw * 0.5 + 0.5) * ScreenSize - 0.5));

                    bKeep = (MinI.x != MaxI.x) && (MinI.y != MaxI.y);
                }

                if (bKeep && IsHiZOccluded(Cache.WorldCenter, Cache.WorldRadius))
                {
                    R.bDefer        = true;
                    R.bDeferCascade = false;
                    bKeep = false;
                }
            }
        }
    }

    R.bEmit = bKeep;
    return R;
}
