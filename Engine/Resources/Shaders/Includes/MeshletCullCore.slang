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
    // True when the meshlet spheres describe this frame's pose, which every size reject below needs.
    bool     bPosedBounds;
    // Folds in the range start, so it indexes SkinnedMeshletBounds() by a mesh-global meshlet index.
    uint     SkinnedBoundsBase;
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

// Every FGPUInstance by POINTER, never by value. It no longer carries a FMeshletHeader* -- a header is a
// slot now -- but MakeMeshletCullContext is still reached through one, and the rule is worth keeping:
// a by-value struct holding a PhysicalStorageBuffer pointer makes Slang emit a Function OpVariable
// without the Aliased/Restrict SPIR-V requires. DEVICE_LOST.
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

    Ctx.SkinnedBoundsBase = kNoSkinnedBounds;
    if (Ctx.bSkinned && SkinnedFrameData() != nullptr && SkinnedMeshletBounds() != nullptr)
    {
        // CullInstances already rejected any skinned slot whose frame tag was stale, so this is current.
        Ctx.SkinnedBoundsBase = SkinnedFrameData()[Inst.RetainedSlot].SkinnedBoundsBase;
    }

    Ctx.bPosedBounds = !Ctx.bSkinned || (Ctx.SkinnedBoundsBase != kNoSkinnedBounds);

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
    C.WorldAxis   = float3(0.0, 0.0, 1.0);
    C.bHasCone    = false;
    C.bConeReady  = false;
    return C;
}

// The posed sphere when this frame wrote one, and the bind-pose sphere otherwise.
FMeshletSphere LoadMeshletSphere(FGPUInstance* Inst, uint SkinnedBoundsBase, uint MeshletIdx)
{
    if (SkinnedBoundsBase != kNoSkinnedBounds)
    {
        return SkinnedMeshletBounds()[SkinnedBoundsBase + MeshletIdx];
    }

    return Inst.MeshletHeader.Spheres[MeshletIdx];
}

void RefreshMeshletBounds(FGPUInstance* Inst, FMeshletCullContext Ctx, uint MeshletIdx, inout FMeshletBoundsCache C)
{
    if (MeshletIdx != C.MeshletIdx)
    {
        const FMeshletSphere S = LoadMeshletSphere(Inst, Ctx.SkinnedBoundsBase, MeshletIdx);

        C.MeshletIdx  = MeshletIdx;
        C.WorldCenter = mul(Ctx.Model, float4(S.Center, 1.0)).xyz;
        C.WorldRadius = S.Radius * Ctx.MaxScale;
        C.bConeReady  = false;
        C.bHasCone    = true;   // resolved with the cone itself; the sphere carries no cone bit
    }
}

// Kept out of the branches below so neither loads through the other branch's pointer.
void ApplyMeshletCone(FMeshletCullContext Ctx, FMeshletCone Packed, inout FMeshletBoundsCache C)
{
    const float4 Cone = DecodeMeshletCone(Packed);

    C.WorldAxis  = normalize(mul((float3x3)Ctx.Model, Cone.xyz));
    C.ConeCutoff = Cone.w;
    C.bHasCone   = Cone.w < 1.0;
}

// Split from the sphere because every meshlet of every view pays that load, while this one needs a
// non-skinned, non-two-sided, cone-bearing meshlet in a cone-enabled view.
void EnsureMeshletCone(FGPUInstance* Inst, FMeshletCullContext Ctx, inout FMeshletBoundsCache C)
{
    if (C.bConeReady)
    {
        return;
    }

    C.bConeReady = true;
    C.bHasCone   = false;

    if (Ctx.SkinnedBoundsBase != kNoSkinnedBounds)
    {
        if (SkinnedMeshletCones() == nullptr)
        {
            return;
        }

        // Rewritten per frame against this pose, and packed exactly like the baked one.
        ApplyMeshletCone(Ctx, SkinnedMeshletCones()[Ctx.SkinnedBoundsBase + C.MeshletIdx], C);
        return;
    }

    // Same reasoning as the sphere gate, a header without cones is a meshlet without one.
    if (Inst.MeshletHeader.Cones == nullptr)
    {
        return;
    }

    ApplyMeshletCone(Ctx, Inst.MeshletHeader.Cones[C.MeshletIdx], C);
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
        // Bounded by the HEADER's own count, not the instance's: MeshletTotalCount comes from the surface's
        // LOD table while Spheres/Cones come from the header, and only the header knows how long they are.
        // A mesh that has died leaves the instance naming the null header, whose count is 0, so this is
        // also what stops a stale instance from indexing geometry that is no longer there.
        // The count is the documented bound, but a GPU dump caught a count over a null Spheres.
        bKeep = MeshletIdx < Inst.MeshletHeader.MeshletCount && Inst.MeshletHeader.Spheres != nullptr;

        if (bKeep)
        {
            RefreshMeshletBounds(Inst, Ctx, MeshletIdx, Cache);

            // Ordered cheapest first, so each reject gates the loads and taps the next one would pay for.
            if (Ctx.bPosedBounds && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_FRUSTUM))
            {
                bKeep = FrustumContainsSphere(View.FrustumPlanes, Cache.WorldCenter, Cache.WorldRadius);
            }

            // The ortho box above nests across cascades, so everything that actually separates them is here.
            if (bKeep && !Ctx.bNoCull && View.HasFlag(CULL_VIEW_FLAG_CASCADE))
            {
                // One compare, and it comes from the cascade's own texel pitch, so it leads.
                if (Ctx.bPosedBounds && View.MinBoundsDiameter > 0.0)
                {
                    // Rejects on smallness, so it needs bounds that are tight and not merely valid.
                    bKeep = (Cache.WorldRadius * 2.0) >= View.MinBoundsDiameter;
                }

                if (bKeep)
                {
                    // Falls back to the instance sphere only when no posed bounds exist for this frame.
                    const float3 CullCenter = Ctx.bPosedBounds ? Cache.WorldCenter : Ctx.InstCenter;
                    const float  CullRadius = Ctx.bPosedBounds ? Cache.WorldRadius : Ctx.InstRadius;

                    // The camera slice swept toward the sun, which unlike the ortho box does not nest.
                    bKeep = FrustumContainsSphere(Scene().CullData.CascadeFrustum[View.CascadeIndex],
                                                 CullCenter, CullRadius);
                }
            }

            // Sun views test against the sun direction, everything else against the meshlet's own sphere.
            if (bKeep && Ctx.bPosedBounds && !Ctx.bNoCull && !Ctx.bTwoSided && View.HasFlag(CULL_VIEW_FLAG_CONE))
            {
                EnsureMeshletCone(Inst, Ctx, Cache);

                if (Cache.bHasCone)
                {
                    if (View.HasFlag(CULL_VIEW_FLAG_SUN_ALIGNED))
                    {
                        // Orthographic, so the sphere term the perspective branch needs drops out here.
                        bKeep = dot(GetSunDirection(), Cache.WorldAxis) > -Cache.ConeCutoff;
                    }
                    else
                    {
                        // No apex is stored, so this is meshoptimizer's sphere form of the same reject.
                        const float3 ToCenter    = Cache.WorldCenter - View.ViewOrigin;
                        const float  ToCenterLen = length(ToCenter);

                        // The radius term is what replaces the apex, so it needs the meshlet's own sphere.
                        bKeep = dot(ToCenter, Cache.WorldAxis)
                              < Cache.ConeCutoff * ToCenterLen + Cache.WorldRadius;
                    }
                }
            }

            // Both are camera-view screen tests, and a bind-pose sphere would hide geometry that has moved.
            const bool bScreenTest = Ctx.bPosedBounds && !Ctx.bNoCull;

            // Sub-pixel is a size test against this frame's transforms, so it is exact in both phases.
            const bool bSubPixelTest = bScreenTest && View.HasFlag(CULL_VIEW_FLAG_OCCLUSION);

            // Hi-Z only where the pyramid describes this frame, which is the late phase alone.
            const bool bHiZTest = bScreenTest && bApplyHiZ && View.HasFlag(CULL_VIEW_FLAG_MESHLET_HIZ);

            if (bKeep && (bSubPixelTest || bHiZTest))
            {
                // One view transform and one tangent construction for both tests below.
                const FCullSphereProjection Proj = ProjectCullSphere(Cache.WorldCenter, Cache.WorldRadius);

                if (Proj.bValid)
                {
                    if (bSubPixelTest)
                    {
                        bKeep = !IsSubPixel(Proj);
                    }

                    // Ordered last so the pyramid is tapped only after every cheaper exact test accepted it.
                    if (bKeep && bHiZTest)
                    {
                        bKeep = !IsHiZOccluded(Proj);
                    }
                }
            }
        }
    }

    return bKeep;
}
