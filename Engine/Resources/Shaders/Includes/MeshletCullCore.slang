#pragma once
#include "Culling.slang"

// Rejects set a flag and fall through, since an early exit makes the task stage's DispatchMesh non-uniform and hangs AMD.

// Built once per (instance, walk index) and reused across every view the caller tests.
struct FMeshletCullContext
{
    uint     DrawID;
    // Carried so the header is reached without a second whole-instance load.
    uint     HeaderSlot;

    bool     bSkinned;
    // True when the meshlet spheres describe this frame's pose, which every size reject below needs.
    bool     bPosedBounds;
    // Folds in the range start, so it indexes SkinnedMeshletBounds() by a mesh-global meshlet index.
    uint     SkinnedBoundsBase;
    bool     bNoCull;
    bool     bCaster;
    bool     bFarCaster;
    bool     bShadowOnly;
    bool     bTwoSided;
    float4x4 Model;
    float    MaxScale;

    float3   InstCenter;
    float    InstRadius;
    float    DistToCamera;
    float    MaxShadowDist;
};

// Read once by value, since each call through the pointer loads all 128 bytes.
FMeshletCullContext MakeMeshletCullContext(FGPUInstance* Inst)
{
    const FGPUInstance I = loadAligned<16>(Inst);

    FMeshletCullContext Ctx;
    Ctx.DrawID     = I.DrawID;
    Ctx.HeaderSlot = I.MeshletHeaderSlot;

    Ctx.bSkinned    = I.HasFlag(EInstanceFlags::Skinned);
    Ctx.bNoCull     = I.HasFlag(EInstanceFlags::NoCull);
    Ctx.bCaster     = I.HasFlag(EInstanceFlags::CastShadow);
    Ctx.bFarCaster  = I.HasFlag(EInstanceFlags::CastFarShadow);
    Ctx.bShadowOnly = I.HasFlag(EInstanceFlags::ShadowOnly);
    Ctx.bTwoSided   = I.HasFlag(EInstanceFlags::TwoSided);

    Ctx.Model = I.ModelMatrix;

    const float SqScaleX = dot(Ctx.Model[0].xyz, Ctx.Model[0].xyz);
    const float SqScaleY = dot(Ctx.Model[1].xyz, Ctx.Model[1].xyz);
    const float SqScaleZ = dot(Ctx.Model[2].xyz, Ctx.Model[2].xyz);
    Ctx.MaxScale = sqrt(max(SqScaleX, max(SqScaleY, SqScaleZ)));

    Ctx.SkinnedBoundsBase = kNoSkinnedBounds;
    if (Ctx.bSkinned && SkinnedFrameData().Contains(I.RetainedSlot))
    {
        // CullInstances already rejected any skinned slot whose frame tag was stale, so this is current.
        Ctx.SkinnedBoundsBase = SkinnedFrameData().At(I.RetainedSlot).SkinnedBoundsBase;
    }

    Ctx.bPosedBounds = !Ctx.bSkinned || (Ctx.SkinnedBoundsBase != kNoSkinnedBounds);

    Ctx.InstCenter    = I.BoundsCenter;
    Ctx.InstRadius    = I.BoundsRadius;
    Ctx.DistToCamera  = length(Ctx.InstCenter - GetCullCameraPosition());
    Ctx.MaxShadowDist = Scene().CullData.ShadowMaxDistance;

    return Ctx;
}

// Not hoistable out of a view loop since LODs differ per view; keyed on the resolved index so cascades sharing a LOD load once.
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

// Posed sphere when this frame wrote one; the arena count says whether the folded base landed.
FMeshletSphere LoadMeshletSphere(FMeshletHeader* Header, uint SkinnedBoundsBase, uint MeshletIdx)
{
    if (SkinnedBoundsBase != kNoSkinnedBounds
        && SkinnedMeshletBounds().Contains(SkinnedBoundsBase + MeshletIdx))
    {
        return SkinnedMeshletBounds().GetAligned((SkinnedBoundsBase + MeshletIdx));
    }

    return loadAligned<16>(Header.Spheres + MeshletIdx);
}

void RefreshMeshletBounds(FMeshletHeader* Header, FMeshletCullContext Ctx, uint MeshletIdx, inout FMeshletBoundsCache C)
{
    if (MeshletIdx != C.MeshletIdx)
    {
        const FMeshletSphere S = LoadMeshletSphere(Header, Ctx.SkinnedBoundsBase, MeshletIdx);

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

// Split from the sphere load, which every meshlet of every view pays.
void EnsureMeshletCone(FMeshletHeader* Header, FMeshletCullContext Ctx, inout FMeshletBoundsCache C)
{
    if (C.bConeReady)
    {
        return;
    }

    C.bConeReady = true;
    C.bHasCone   = false;

    // Same arena and same bound as the posed sphere, since one base addresses both.
    if (Ctx.SkinnedBoundsBase != kNoSkinnedBounds
        && SkinnedMeshletCones().Contains(Ctx.SkinnedBoundsBase + C.MeshletIdx))
    {
        // Rewritten per frame against this pose, and packed exactly like the baked one.
        ApplyMeshletCone(Ctx, SkinnedMeshletCones().Get(Ctx.SkinnedBoundsBase + C.MeshletIdx), C);
        return;
    }

    ApplyMeshletCone(Ctx, Header.Cones[C.MeshletIdx], C);
}

// The early phase must not Hi-Z cull, since a meshlet dropped there is never redrawn by the late phase.
bool TestMeshletAgainstView(FMeshletCullContext Ctx, FCullView View,
                            uint MeshletIdx, inout FMeshletBoundsCache Cache, bool bApplyHiZ)
{
    FMeshletHeader* Header = MeshletHeaders().At(Ctx.HeaderSlot);

    // Must agree with IsInstanceVisibleToView, which sized the bucket these appends fill.
    const bool bShadowView = View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY);

    bool bKeep = !(bShadowView && !Ctx.bCaster)
              && !(View.HasFlag(CULL_VIEW_FLAG_FAR_CASCADE) && !Ctx.bFarCaster)
              && !(!bShadowView && Ctx.bShadowOnly)
              && !(View.HasFlag(CULL_VIEW_FLAG_DISTANCE) && (Ctx.DistToCamera - Ctx.InstRadius > Ctx.MaxShadowDist));

    if (bKeep)
    {
        // Bounded by the header's count, which is 0 for a dead mesh's null header, so stale instances cannot index freed geometry.
        bKeep = MeshletIdx < Header.MeshletCount;

        if (bKeep)
        {
            RefreshMeshletBounds(Header, Ctx, MeshletIdx, Cache);

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
                EnsureMeshletCone(Header, Ctx, Cache);

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
