#pragma once
#include "Culling.slang"

// RULE: NO EARLY RETURNS, NO continue -- every reject sets a flag and falls through. A task stage ends
// in a group barrier and uniform DispatchMesh; a lane that left early makes it non-uniform and AMD hangs.

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

// Every FGPUInstance by POINTER, never by value: it carries a FMeshletHeader*, and a by-value param
// makes Slang emit a Function OpVariable without the Aliased/Restrict SPIR-V requires. DEVICE_LOST.
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

// Cannot be hoisted out of a view loop: two views at different LODs address different meshlets at the
// same walk index. Cached on the RESOLVED index, so cascades sharing a shadow LOD still load once.
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

// A pure function of (meshlet, view) apart from bApplyHiZ, which is the caller's phase decision: the
// early phase replays last frame's visible set and must NOT Hi-Z cull it (a meshlet dropped there is
// dropped for good, because the late phase skips instances the early phase drew), while the late phase
// tests against the pyramid just rebuilt from the early phase's depth.
bool TestMeshletAgainstView(FMeshletCullContext Ctx, FGPUInstance* Inst, FCullView View,
                            uint MeshletIdx, inout FMeshletBoundsCache Cache, bool bApplyHiZ)
{
    bool bKeep = !(View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY) && !Ctx.bCaster)
              && !(View.HasFlag(CULL_VIEW_FLAG_DISTANCE) && (Ctx.DistToCamera - Ctx.InstRadius > Ctx.MaxShadowDist));

    if (bKeep)
    {
        // Unreachable (the block already clamped the range), but an unbounded index walks the header arrays
        // past the allocation, and scattered BDA reads return garbage rather than faulting: a TDR with no fault.
        bKeep = MeshletIdx < Inst.MeshletTotalCount;

        if (bKeep)
        {
            RefreshMeshletBounds(Inst, Ctx.Model, Ctx.MaxScale, MeshletIdx, Cache);

            // Sun views test against the sun direction; everything else against apex-to-origin.
            if (!Ctx.bSkinned && !Ctx.bNoCull && !Ctx.bTwoSided && View.HasFlag(CULL_VIEW_FLAG_CONE))
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

            if (bKeep && !Ctx.bSkinned && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_FRUSTUM))
            {
                bKeep = FrustumContainsSphere(View.FrustumPlanes, Cache.WorldCenter, Cache.WorldRadius);
            }

            // The frustum test above is the cascade's ortho box, and those nest, so it barely separates
            // one cascade from the next. Everything that makes a cascade cheap is here.
            if (bKeep && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_CASCADE))
            {
                const uint CascadeIndex = View.CascadeIndex;

                // A skinned mesh's MESHLET bounds are bind-pose; the INSTANCE sphere is re-fed each frame. Coarser
                // but true rather than stale -- the meshlet sphere punched pose-dependent holes in character shadows.
                const float3 CullCenter = Ctx.bSkinned ? Ctx.InstCenter : Cache.WorldCenter;
                const float  CullRadius = Ctx.bSkinned ? Ctx.InstRadius : Cache.WorldRadius;

                // The camera slice swept toward the sun. Unlike the ortho box it does not nest, which is
                // what stops cascade 3 re-rastering cascade 0's scene.
                bKeep = FrustumContainsSphere(Scene().CullData.CascadeFrustum[CascadeIndex], CullCenter, CullRadius);

                // MinBoundsDiameter comes from the cascade's own texel pitch, so a meshlet can survive cascade 0 and
                // die in cascade 3. Skinned is excluded, not switched: this rejects on SMALLNESS.
                if (bKeep && !Ctx.bSkinned && View.MinBoundsDiameter > 0.0)
                {
                    bKeep = (Cache.WorldRadius * 2.0) >= View.MinBoundsDiameter;
                }
            }

            // Sub-pixel reject, camera view only. A projected-SIZE test against THIS frame's transforms,
            // so it is exact in both phases -- see the note on CULL_VIEW_FLAG_OCCLUSION in Common.slang.
            if (bKeep && !Ctx.bSkinned && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_OCCLUSION))
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

            // Last, so the pyramid is tapped only after every cheaper exact test accepted it. Skinned
            // and NoCull are excluded -- a bind-pose sphere can hide geometry that has since moved.
            if (bApplyHiZ && bKeep && !Ctx.bSkinned && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_MESHLET_HIZ))
            {
                bKeep = !IsHiZOccluded(Cache.WorldCenter, Cache.WorldRadius);
            }
        }
    }

    return bKeep;
}
