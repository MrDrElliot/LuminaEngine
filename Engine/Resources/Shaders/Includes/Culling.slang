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

// 2D polyhedral bounds of a clipped perspective-projected sphere (Mara/McGuire 2013), left-handed
// view space. Returns false if the sphere clips the near plane -- caller treats it as visible.
bool ProjectSphereClip(float3 C, float R, float ZNear, float P00, float P11, out float4 ClipAABB)
{
    ClipAABB = float4(-1.0, -1.0, 1.0, 1.0);

    if (C.z - R < ZNear)
    {
        return false;
    }

    // Orthographic: rays are parallel, so the silhouette is a plain scale of the world radius. The
    // tangent construction below divides by view Z and assumes rays converging on the eye.
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

// Per-view LOD. The pick is made TWICE per (instance, view) -- CullInstances to reserve draw-list
// space, BuildMeshletBlocks to walk -- so it lives here and is CALLED by both, never re-derived.
uint SelectSurfaceLODForView(FSurfaceDesc Desc, float3 ViewOrigin, float4 SphereBounds)
{
    const float3 ToView  = SphereBounds.xyz - ViewOrigin;
    const float  DistSq  = dot(ToView, ToView);
    const uint   NumLODs = min(Desc.NumLODs, (uint)MESHLET_MAX_LODS);
    const uint   Picked  = Desc.SelectLOD(DistSq, SphereBounds.w * SphereBounds.w);

    // NumLODs indexes MESHLET_MAX_LODS-long arrays, so clamp here: past the table the offsets are
    // whatever follows the allocation, and both the dispatch domain and the reservation sum from them.
    return min(Picked, NumLODs > 0u ? NumLODs - 1u : 0u);
}

// The meshlet range that pick resolves to: xy = (mesh-global offset, count).
uint2 SelectViewMeshletRange(FSurfaceDesc Desc, float3 ViewOrigin, float4 SphereBounds)
{
    const uint LOD = SelectSurfaceLODForView(Desc, ViewOrigin, SphereBounds);
    return uint2(Desc.LODMeshletOffset[LOD], Desc.LODMeshletCount[LOD]);
}

// One LOD step per cascade index: each cascade's world texel is correspondingly wider. Same
// reserve/walk contract as SelectSurfaceLODForView, so it too is CALLED by both, never re-derived.
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

// Keeps the projected bounds conservative against the pyramid's own quantization.
static const float kCullSphereDilation = 1.02;

// One projection of a cull sphere, shared so the sub-pixel and Hi-Z tests stop repeating each other.
struct FCullSphereProjection
{
    float3 ViewCenter;
    float  ViewRadius;
    float4 ClipAABB;
    bool   bValid;      // false when the sphere straddles the near plane, which both tests read as visible
};

// The cull camera, not the live one, is what the pyramid was rendered from while culling is frozen.
FCullSphereProjection ProjectCullSphere(float3 CenterWS, float RadiusWS)
{
    const float4x4 CullProj = GetCullCameraProjection();

    FCullSphereProjection P;
    P.ViewCenter = mul(GetCullCameraView(), float4(CenterWS, 1.0)).xyz;
    P.ViewRadius = RadiusWS * kCullSphereDilation;
    P.bValid     = ProjectSphereClip(P.ViewCenter, P.ViewRadius, Scene().NearPlane,
                                     CullProj[0][0], CullProj[1][1], P.ClipAABB);
    return P;
}

// A sphere whose screen AABB lands on one pixel center in either axis rasterizes nothing.
bool IsSubPixel(FCullSphereProjection P)
{
    const float2 ScreenSize = float2(Scene().ScreenSize.xy);
    const int2   MinI = int2(floor((P.ClipAABB.xy * 0.5 + 0.5) * ScreenSize - 0.5));
    const int2   MaxI = int2(floor((P.ClipAABB.zw * 0.5 + 0.5) * ScreenSize - 0.5));

    return (MinI.x == MaxI.x) || (MinI.y == MaxI.y);
}

// Returns true if the sphere is fully occluded by the camera depth pyramid and can be skipped.
bool IsHiZOccluded(FCullSphereProjection P)
{
    const float ZNear = Scene().NearPlane;
    const float ZFar  = Scene().FarPlane;

    // Clip [-1,1] to Vulkan UV [0,1], direct because the projection already bakes +Y-down NDC.
    const float4 UvAABB = saturate(P.ClipAABB * 0.5 + 0.5);

    const float Width  = (UvAABB.z - UvAABB.x) * Scene().CullData.PyramidWidth;
    const float Height = (UvAABB.w - UvAABB.y) * Scene().CullData.PyramidHeight;

    // Mip where a single bilinear tap (2x2) covers the AABB.
    const float Level = max(ceil(log2(max(Width, Height) * 0.5)), 0.0);

    const float2 SampleUV = (UvAABB.xy + UvAABB.zw) * 0.5;

    // The min-reduction sampler returns the 2x2 minimum, which under reverse-Z is the farthest occluder.
    const float PyramidDepth = SampleTexture2DLevel(Scene().CullData.DepthPyramidIndex, SAMPLER_MIN_REDUCTION, SampleUV, Level).x;

    const float FrontZ      = P.ViewCenter.z - P.ViewRadius;
    const float DepthSphere = SphereFrontDepth(FrontZ, ZNear, ZFar);

    // Reverse-Z, so occluded means the sphere's nearest depth sits behind that occluder.
    return DepthSphere < PyramidDepth;
}

// Tested against the PREVIOUS frame's cascade depth, which is STANDARD Z, so the footprint reduction
// is a MAX not a min. bMid selects which pyramid -- and so which transforms; never choose separately.
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

// The per-(instance, view) reject in ONE place: CullInstances sizes each bucket from this and
// BuildMeshletBlocks appends against that sizing, so a disagreement silently drops an instance.
bool IsInstanceVisibleToView(FCullView View, float3 Center, float Radius, bool bNoCull, bool bCaster)
{
    // Outside the NoCull gate: NoCull suppresses visibility culling, not whether the instance casts.
    if (View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY) && !bCaster)
    {
        return false;
    }

    if (bNoCull)
    {
        return true;
    }

    if (View.HasFlag(CULL_VIEW_FLAG_FRUSTUM) && !FrustumContainsSphere(View.FrustumPlanes, Center, Radius))
    {
        return false;
    }

    if (View.HasFlag(CULL_VIEW_FLAG_CASCADE))
    {
        // The cascade's own ortho box barely separates one cascade from the next (the fitted spheres
        // nest); CascadeFrustum is the camera slice swept toward the sun, which does not.
        if (!FrustumContainsSphere(Scene().CullData.CascadeFrustum[View.CascadeIndex], Center, Radius))
        {
            return false;
        }
        if (Scene().CullData.bShadowOcclusionCull != 0u &&
            IsCascadeHZBOccluded(View.CascadeIndex, Center, Radius))
        {
            return false;
        }
    }

    // No camera Hi-Z test here. This pass runs before the frame's own pyramid exists, so it could only
    // sample LAST frame's -- and a reject leaves the instance with no blocks at all, which neither cull
    // phase can undo. That is the frame of lag the two-phase occlusion exists to remove. The camera
    // occlusion test now lives in MeshletCull's late phase, against the pyramid built this frame.
    // Cascades keep theirs above: shadow views are single-phase and have no second chance to gain from.
    return true;
}
