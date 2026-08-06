#pragma once
#include "SceneGlobals.slang"

// Helpers shared by the GPU culling passes, centralized so the Hi-Z math stays in sync
// with how the depth pyramid is generated (MIN-reduction sampler, reverse-Z).

bool FrustumContainsSphere(FFrustum F, float3 Center, float Radius)
{
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float4 Plane = F.Planes[i];
        float  Dist  = dot(Plane.xyz, Center) + Plane.w;
        if (Dist < -Radius)
        {
            return false;
        }
    }
    return true;
}

// Overload for FCullView-style flat plane arrays (float4 Planes[6]).
bool FrustumContainsSphere(float4 Planes[6], float3 Center, float Radius)
{
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float Dist = dot(Planes[i].xyz, Center) + Planes[i].w;
        if (Dist < -Radius)
        {
            return false;
        }
    }
    return true;
}

// 2D polyhedral bounds of a clipped perspective-projected sphere (Mara/McGuire 2013), for
// left-handed view space (camera down +Z). ClipAABB = (minX, minY, maxX, maxY), each /clip.w.
// Returns false if the sphere clips the near plane (caller treats the object as visible).
bool ProjectSphereClip(float3 C, float R, float ZNear, float P00, float P11, out float4 ClipAABB)
{
    ClipAABB = float4(-1.0, -1.0, 1.0, 1.0);

    if (C.z - R < ZNear)
    {
        return false;
    }

    // Orthographic: view rays are parallel, so the silhouette is a plain scale of the world radius
    // (same as the cascade path below) and the tangent construction underneath -- which divides by
    // view Z and so assumes rays converging on the eye -- does not apply. Both callers pass the
    // camera projection, which is what IsCameraOrthographic describes.
    if (IsCameraOrthographic())
    {
        float2 Center = float2(C.x * P00, C.y * P11);
        float2 Extent = float2(R * abs(P00), R * abs(P11));
        ClipAABB = float4(Center - Extent, Center + Extent);
        return true;
    }

    float2 cx = float2(C.x, C.z);
    float  tx = sqrt(dot(cx, cx) - R * R);
    float2 minX = float2(tx * cx.x - R * cx.y,  R * cx.x + tx * cx.y);
    float2 maxX = float2(tx * cx.x + R * cx.y, -R * cx.x + tx * cx.y);

    float2 cy = float2(C.y, C.z);
    float  ty = sqrt(dot(cy, cy) - R * R);
    float2 minY = float2(ty * cy.x - R * cy.y,  R * cy.x + ty * cy.y);
    float2 maxY = float2(ty * cy.x + R * cy.y, -R * cy.x + ty * cy.y);

    float2 xPair = float2(minX.x / minX.y, maxX.x / maxX.y) * P00;
    float2 yPair = float2(minY.x / minY.y, maxY.x / maxY.y) * P11;

    // P11 is negative (Vulkan +Y-down projection), which inverts the order of
    // the y-axis pair. Sort so callers can always assume xy = min, zw = max.
    ClipAABB = float4(
        min(xPair.x, xPair.y),
        min(yPair.x, yPair.y),
        max(xPair.x, xPair.y),
        max(yPair.x, yPair.y)
    );

    return true;
}

// Reverse-Z depth matching glm::perspective(fov, asp, Far, Near):
// depth(z) = (N * F / z - N) / (F - N).
float SphereFrontDepth(float FrontZ, float ZNear, float ZFar)
{
    // Reverse-Z ortho is linear in view Z: depth(z) = (z - F) / (N - F).
    if (IsCameraOrthographic())
    {
        return (FrontZ - ZFar) / (ZNear - ZFar);
    }

    return (ZNear * ZFar / FrontZ - ZNear) / (ZFar - ZNear);
}

// ---------------------------------------------------------------------------------------------------
// Per-view LOD selection
// ---------------------------------------------------------------------------------------------------
//
// A view's LOD is a function of that view's own origin, so a preview camera standing next to a mesh sees
// it at the detail IT needs rather than at whatever the primary camera picked. Which means the pick is
// made twice per (instance, view): CullInstances makes it to reserve draw-list space, CullMeshlets makes
// it again to decide which meshlets that view walks. The two MUST return the same LOD -- reserve for LOD 2
// and walk LOD 1 and the surplus is wasted; the other way round and meshlets are silently dropped at the
// region boundary. That is why the arithmetic lives here and is CALLED by both, never re-derived: same
// expression, same operand order, so the float math is bit-identical on both sides.
//
// Shadow views deliberately do NOT go through this. Cascades are fit to the primary camera, so the LOD
// that matches their texel pitch is a function of the primary camera's distance; the instance carries
// that pick pre-made in ShadowMeshletOffset/Count.
uint SelectSurfaceLODForView(FSurfaceDesc Desc, float3 ViewOrigin, float4 SphereBounds)
{
    const float3 ToView  = SphereBounds.xyz - ViewOrigin;
    const float  DistSq  = dot(ToView, ToView);
    const uint   NumLODs = min(Desc.NumLODs, (uint)MESHLET_MAX_LODS);
    const uint   Picked  = Desc.SelectLOD(DistSq, SphereBounds.w * SphereBounds.w);

    // NumLODs comes through the desc and indexes MESHLET_MAX_LODS-long arrays, so clamp it here: past the
    // table the offsets and counts below are whatever follows the allocation, and a garbage meshlet count
    // is what both the dispatch domain and the draw-list reservation are summed from.
    return min(Picked, NumLODs > 0u ? NumLODs - 1u : 0u);
}

// The meshlet range that pick resolves to: xy = (mesh-global offset, count).
uint2 SelectViewMeshletRange(FSurfaceDesc Desc, float3 ViewOrigin, float4 SphereBounds)
{
    const uint LOD = SelectSurfaceLODForView(Desc, ViewOrigin, SphereBounds);
    return uint2(Desc.LODMeshletOffset[LOD], Desc.LODMeshletCount[LOD]);
}

// The shadow LOD a CASCADE renders an instance at.
//
// Cascade i covers a slice roughly twice as deep as cascade i-1 at the SAME atlas resolution, so its world
// texel is correspondingly wider and the geometry that fills one texel is correspondingly coarser. One LOD
// step per cascade index tracks that: cascade 0 renders what the old single shadow pick rendered (step 0,
// bit-identical), and each further cascade drops one more level. Everything the far cascades save comes out
// of geometry that was already narrower than their own texel.
//
// Same contract as SelectSurfaceLODForView: CullInstances calls this to RESERVE the (view, draw) region and
// CullMeshlets calls it to decide which meshlets that cascade walks. Reserve for one LOD and walk another
// and the surplus is wasted (or, the other way, meshlets vanish at the region boundary) -- so the
// arithmetic lives here and is CALLED by both, never re-derived.
//
// Only for views flagged Cascade AND selectable (SurfaceDescIndex != kNoSurfaceDesc). Point/spot shadow
// views and fixed-pick instances (skinned, forced LOD, LODs off) keep FGPUInstance's single shadow range.
uint SelectCascadeShadowLOD(FSurfaceDesc Desc, float3 CamPos, float4 SphereBounds, uint CascadeIndex,
                            int ShadowLODBias, float ShadowCoarseLODDistSq)
{
    const uint   NumLODs = min(Desc.NumLODs, (uint)MESHLET_MAX_LODS);
    const float3 ToCam   = SphereBounds.xyz - CamPos;
    const float  DistSq  = dot(ToCam, ToCam);

    // The camera-distance pick the shadow LOD has always been biased off. Same expression and operand
    // order as SelectSurfaceLODForView so the two stay bit-identical on both sides.
    const uint CameraLOD = Desc.SelectLOD(DistSq, SphereBounds.w * SphereBounds.w);

    // Sloppy LODs (4-5) can hole and light-leak; past ShadowCoarseLODDistance the cascade texel is wide
    // enough that the holes stay sub-texel. Mirrors CullInstances' ShadowCap exactly.
    const uint Cap = (ShadowCoarseLODDistSq > 0.0 && DistSq >= ShadowCoarseLODDistSq)
                   ? (uint)MESHLET_MAX_COARSE_SHADOW_LOD : (uint)MESHLET_MAX_SHADOW_LOD;
    const int  MaxLOD = (int)min(NumLODs > 0u ? NumLODs - 1u : 0u, Cap);

    return (uint)clamp((int)CameraLOD + ShadowLODBias + (int)CascadeIndex, 0, MaxLOD);
}

// The meshlet range that cascade pick resolves to: xy = (mesh-global offset, count).
uint2 SelectCascadeShadowMeshletRange(FSurfaceDesc Desc, float3 CamPos, float4 SphereBounds,
                                      uint CascadeIndex, int ShadowLODBias, float ShadowCoarseLODDistSq)
{
    const uint LOD = SelectCascadeShadowLOD(Desc, CamPos, SphereBounds, CascadeIndex,
                                            ShadowLODBias, ShadowCoarseLODDistSq);
    return uint2(Desc.LODMeshletOffset[LOD], Desc.LODMeshletCount[LOD]);
}

// Tests a world-space sphere against the camera depth pyramid.
// Returns true if the sphere is fully occluded and can be skipped.
bool IsHiZOccluded(float3 CenterWS, float RadiusWS)
{
    float  ZNear = Scene().NearPlane;
    float  ZFar  = Scene().FarPlane;

    float3 ViewCenter = mul(Scene().CameraView.CameraView, float4(CenterWS, 1.0)).xyz;
    float  ViewRadius = RadiusWS * 1.02; // Tiny dilation for conservatism.

    float4 ClipAABB;
    if (!ProjectSphereClip(ViewCenter, ViewRadius, ZNear,
                           Scene().CameraView.CameraProjection[0][0],
                           Scene().CameraView.CameraProjection[1][1],
                           ClipAABB))
    {
        // Sphere straddles the near plane: treat as visible.
        return false;
    }

    // Clip [-1,1] -> Vulkan UV [0,1]. The camera projection bakes Vulkan
    // +Y-down NDC, so this is a direct mapping with no per-axis swap.
    float4 UvAABB = saturate(ClipAABB * 0.5 + 0.5);

    float Width  = (UvAABB.z - UvAABB.x) * Scene().CullData.PyramidWidth;
    float Height = (UvAABB.w - UvAABB.y) * Scene().CullData.PyramidHeight;

    // Pick a mip where a single bilinear tap (2x2) covers the AABB.
    float Level = max(ceil(log2(max(Width, Height) * 0.5)), 0.0);

    float2 SampleUV = (UvAABB.xy + UvAABB.zw) * 0.5;

    // Bindless Hi-Z tap with the MIN-reduction sampler returns the minimum
    // depth of the 2x2 footprint. In reverse-Z, min == farthest.
    float PyramidDepth = SampleTexture2DLevel(Scene().CullData.DepthPyramidIndex, SAMPLER_MIN_REDUCTION, SampleUV, Level).x;

    float FrontZ      = ViewCenter.z - ViewRadius;
    float DepthSphere = SphereFrontDepth(FrontZ, ZNear, ZFar);

    // Reverse-Z: occluded iff the sphere's nearest depth is farther than the
    // farthest occluder in its footprint.
    return DepthSphere < PyramidDepth;
}

// Rejects a caster whose shadow is already fully written by something nearer the light, tested against the
// PREVIOUS frame's cascade depth. Differs from the camera HZB in two ways that matter:
//
//   * the cascade atlas is STANDARD Z (Less test, cleared to 1.0), so "the farthest occluder in the
//     footprint" is a MAX, not a min -- hence SAMPLER_MAX_REDUCTION and a max-reduced pyramid.
//   * the projection is orthographic, so the sphere's NDC footprint is a plain scale of its world radius
//     (CascadeHZBNdcScale) rather than the perspective sphere-clip the camera path needs.
//
// Conservative by construction: a sphere that leaves the cascade's clip box skips the test entirely, and
// the tap is allowed to bleed into a neighbouring cascade's tile because sampling a SUPERSET of the
// footprint can only ever raise the max, i.e. cull less.
// bMid selects WHICH cascade pyramid, and so which transforms: false = the one built at the end of last
// frame (what phase 0 tests), true = the one rebuilt mid-frame from this frame's own cascade raster (what
// phase 2 tests). The two carry different matrices whenever the camera moved, so the choice of pyramid and
// the choice of transform have to be made together, never independently.
bool IsCascadeHZBOccluded(uint CascadeIndex, float3 CenterWS, float RadiusWS, bool bMid = false)
{
    const uint bValid = bMid ? Scene().CullData.bCascadeHZBMidValid : Scene().CullData.bCascadeHZBValid;
    if (bValid == 0u || CascadeIndex >= (uint)NUM_CASCADES)
    {
        return false;
    }

    const float4x4 CascadeVP = bMid ? Scene().CullData.CascadeHZBViewProjectionMid[CascadeIndex]
                                    : Scene().CullData.CascadeHZBViewProjection[CascadeIndex];
    const float3   NdcScale  = bMid ? Scene().CullData.CascadeHZBNdcScaleMid[CascadeIndex].xyz
                                    : Scene().CullData.CascadeHZBNdcScale[CascadeIndex].xyz;

    // Ortho: w == 1, so clip space is already NDC.
    const float3 Ndc       = mul(CascadeVP, float4(CenterWS, 1.0)).xyz;
    const float3 NdcRadius = RadiusWS * NdcScale;

    const float2 MinNdc = Ndc.xy - NdcRadius.xy;
    const float2 MaxNdc = Ndc.xy + NdcRadius.xy;
    const float  NearZ  = Ndc.z - NdcRadius.z;

    // Partly outside the cascade volume: the raster clips it, so the pyramid never described all of it.
    // Treat as visible rather than guess.
    if (any(MinNdc < -1.0) || any(MaxNdc > 1.0) || NearZ < 0.0)
    {
        return false;
    }

    // NDC -> tile-local UV -> pyramid UV. The ortho projection already bakes Vulkan +Y-down, so this is a
    // direct mapping, same as the camera path.
    const float4 Tile     = Scene().CullData.CascadeHZBTile[CascadeIndex];
    const float2 LocalMin = MinNdc * 0.5 + 0.5;
    const float2 LocalMax = MaxNdc * 0.5 + 0.5;

    const float2 UvMin = Tile.xy + LocalMin * Tile.zw;
    const float2 UvMax = Tile.xy + LocalMax * Tile.zw;

    const float Width  = (UvMax.x - UvMin.x) * Scene().CullData.CascadePyramidWidth;
    const float Height = (UvMax.y - UvMin.y) * Scene().CullData.CascadePyramidHeight;

    // Mip where one bilinear tap (2x2) covers the footprint, clamped to the chain we actually allocated.
    const float MaxLevel = max((float)Scene().CullData.CascadePyramidMipCount - 1.0, 0.0);
    const float Level    = clamp(ceil(log2(max(max(Width, Height) * 0.5, 1.0))), 0.0, MaxLevel);

    const float2 SampleUV = (UvMin + UvMax) * 0.5;

    // MIN-reduction is what the camera's reverse-Z pyramid wants; standard Z wants the opposite.
    const float PyramidDepth = SampleTexture2DLevel(Scene().CullData.CascadePyramidIndex, SAMPLER_MAX_REDUCTION, SampleUV, Level).x;

    // Standard Z: occluded iff the whole sphere sits behind the farthest thing already in its footprint,
    // i.e. every fragment it would emit fails the Less test.
    return NearZ > PyramidDepth;
}
