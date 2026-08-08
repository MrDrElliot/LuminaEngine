#pragma once
#include "Culling.slang"

// The per-meshlet cull body, inlined into every geometry pass's task (amplification) stage.
//
// RULE: NO EARLY RETURNS, NO `continue` -- every reject sets a flag and falls through. A task shader ends
// in a group barrier and a uniform DispatchMesh, and a lane that returned before the barrier makes it
// non-uniform: undefined behaviour NVIDIA tolerates and AMD hangs on, with no page fault and no failing
// lane to inspect. Nested predicates cost nothing; the compiler flattens them the same way.

// Built once per (instance, walk index) and reused across every view the caller tests.
struct FMeshletCullContext
{
    uint     DrawID;

    // Unpacked once rather than re-tested per view.
    bool     bSkinned;
    bool     bNoCull;
    bool     bCaster;
    bool     bTwoSided;
    float4x4 Model;
    float    MaxScale;

    float3   InstCenter;
    float    InstRadius;
    float    DistToCamera;
    float    MaxShadowDist;
};

// Every FGPUInstance here is taken by POINTER, never by value: it carries a FMeshletHeader*, and a
// by-value parameter makes Slang materialize that pointer into a Function OpVariable which SPIR-V
// requires to carry Aliased or Restrict and Slang emits with neither. Invalid module, accepted by every
// driver, surfacing frames later as VK_ERROR_DEVICE_LOST with no shader attribution.
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
    Ctx.DistToCamera  = length(Ctx.InstCenter - GetCullCameraPosition());
    Ctx.MaxShadowDist = Scene().CullData.ShadowMaxDistance;

    return Ctx;
}

// Nothing here can be hoisted out of a view loop: two views at different LODs address different meshlets
// at the same walk index. Cached on the RESOLVED index instead, so four cascades sharing a shadow LOD
// still load once. One entry deep, which is all the ordering of BuildCullViews makes useful.
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

// Split from the sphere because every meshlet of every view pays that load, while this one needs a
// non-skinned, non-two-sided, cone-bearing meshlet in a cone-enabled view.
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

struct FMeshletCullResult
{
    bool bKeep;       // survives every test this phase can apply -- rasterize it now
    // Rejected by the EARLY Hi-Z test alone, having passed everything else. The late phase re-tests
    // exactly these against the rebuilt pyramid. Never set in the late phase: by then the pyramid holds
    // this frame's own depth and the answer is final.
    bool bDeferHiZ;
};

// One meshlet, one view, one phase. Nested predicates rather than early exits -- see the RULE at the top.
//
// Hi-Z is the ONLY test here whose answer depends on which phase is running, because it is the only one
// that reads state from outside this frame's transforms. Everything else -- cone, frustum, cascade,
// sub-pixel -- is a pure function of (meshlet, view) and cannot change between the two phases. So the
// late phase skips them outright: re-running them would burn the whole cull for a result already known.
//
// That is also why the late phase does not re-derive which lanes to run. The caller seeds it from the
// early phase's defer bits; this function only decides whether the rebuilt pyramid still hides them.
FMeshletCullResult TestMeshletAgainstView(FMeshletCullContext Ctx, FGPUInstance* Inst, FCullView View,
                                          uint MeshletIdx, inout FMeshletBoundsCache Cache, uint Phase)
{
    FMeshletCullResult Result;
    Result.bKeep     = false;
    Result.bDeferHiZ = false;

    const bool bLate = (Phase == CULL_PHASE_LATE);

    // The late phase inherits the early phase's verdict on all of these, so it starts from "accepted"
    // and only the Hi-Z block below can still reject.
    bool bKeep = bLate
               || (!(View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY) && !Ctx.bCaster)
                && !(View.HasFlag(CULL_VIEW_FLAG_DISTANCE) && (Ctx.DistToCamera - Ctx.InstRadius > Ctx.MaxShadowDist)));

    if (bKeep)
    {
        // Unreachable (the block already clamped the range), but an unbounded index walks the header
        // arrays past the allocation, and scattered BDA reads return garbage rather than faulting: a
        // multi-second hang and a TDR with no page fault to point at.
        bKeep = MeshletIdx < Inst.MeshletTotalCount;

        if (bKeep)
        {
            // Both phases need the bounds: the late phase skips every test below but still feeds the
            // Hi-Z tap from this cache.
            RefreshMeshletBounds(Inst, Ctx.Model, Ctx.MaxScale, MeshletIdx, Cache);

            // Sun views test against the sun direction; everything else against apex-to-origin.
            if (!bLate && !Ctx.bSkinned && !Ctx.bNoCull && !Ctx.bTwoSided && View.HasFlag(CULL_VIEW_FLAG_CONE))
            {
                EnsureMeshletCone(Inst, Ctx.Model, Cache);

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

            if (!bLate && bKeep && !Ctx.bSkinned && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_FRUSTUM))
            {
                bKeep = FrustumContainsSphere(View.FrustumPlanes, Cache.WorldCenter, Cache.WorldRadius);
            }

            // The frustum test above is the cascade's ortho box, and those nest, so it barely separates
            // one cascade from the next. Everything that makes a cascade cheap is here.
            if (!bLate && bKeep && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_CASCADE))
            {
                const uint CascadeIndex = View.CascadeIndex;

                // A skinned mesh's MESHLET bounds are bind-pose; the INSTANCE sphere is re-fed each frame
                // and does track it. Coarser, but a true bound rather than a stale one -- the meshlet
                // sphere punched pose-dependent holes in character shadows.
                const float3 CullCenter = Ctx.bSkinned ? Ctx.InstCenter : Cache.WorldCenter;
                const float  CullRadius = Ctx.bSkinned ? Ctx.InstRadius : Cache.WorldRadius;

                // The camera slice swept toward the sun. Unlike the ortho box it does not nest, which is
                // what stops cascade 3 re-rastering cascade 0's scene.
                bKeep = FrustumContainsSphere(Scene().CullData.CascadeFrustum[CascadeIndex], CullCenter, CullRadius);

                // MinBoundsDiameter comes from the cascade's own texel pitch, so the same meshlet
                // survives in cascade 0 and dies in cascade 3. Skinned is excluded rather than switched
                // to the instance sphere: this rejects on SMALLNESS, and that sphere is the whole
                // character, so it would never reject anything.
                if (bKeep && !Ctx.bSkinned && View.MinBoundsDiameter > 0.0)
                {
                    bKeep = (Cache.WorldRadius * 2.0) >= View.MinBoundsDiameter;
                }
            }

            // Sub-pixel reject, camera view only. A projected-SIZE test against THIS frame's transforms,
            // so it is exact and never deferred -- see the note on CULL_VIEW_FLAG_OCCLUSION in Common.slang.
            if (!bLate && bKeep && !Ctx.bSkinned && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_OCCLUSION))
            {
                float4 ClipAABB;
                const float4x4 CullProj = GetCullCameraProjection();
                if (ProjectSphereClip(mul(GetCullCameraView(), float4(Cache.WorldCenter, 1.0)).xyz,
                                      Cache.WorldRadius,
                                      Scene().NearPlane,
                                      CullProj[0][0],
                                      CullProj[1][1],
                                      ClipAABB))
                {
                    const float2 ScreenSize = float2(Scene().ScreenSize.xy);
                    const int2   MinI = int2(floor((ClipAABB.xy * 0.5 + 0.5) * ScreenSize - 0.5));
                    const int2   MaxI = int2(floor((ClipAABB.zw * 0.5 + 0.5) * ScreenSize - 0.5));

                    bKeep = (MinI.x != MaxI.x) && (MinI.y != MaxI.y);
                }
            }

            // Per-meshlet Hi-Z. Last, so a meshlet only ever reaches the defer list once every cheaper
            // and exact test has already accepted it -- deferring something the frustum would have
            // rejected would pay for it twice and re-test it against a pyramid that cannot save it.
            //
            // Skinned and NoCull are excluded for the same reason the frustum test excludes them: the
            // meshlet sphere is bind-pose and does not track the animated silhouette, so an occlusion
            // verdict drawn from it can hide geometry that has since moved out from behind the occluder.
            // The instance-level Hi-Z in IsInstanceVisibleToView still covers those, off the live sphere.
            if (bKeep && !Ctx.bSkinned && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_MESHLET_HIZ))
            {
                if (IsHiZOccluded(Cache.WorldCenter, Cache.WorldRadius))
                {
                    bKeep = false;
                    // Early: hand it to the late phase, which re-tests against this frame's own depth.
                    // Late: the pyramid already holds that depth, so this verdict is final and the
                    // meshlet is genuinely behind something. Deferring again would loop forever.
                    Result.bDeferHiZ = !bLate;
                }
            }
        }
    }

    Result.bKeep = bKeep;
    return Result;
}
