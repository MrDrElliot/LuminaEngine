#pragma once
#include "SceneGlobals.slang"

// Hi-Z math here must match the depth pyramid build (min-reduction sampler, reverse-Z).

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

// Mara/McGuire 2013 sphere bounds in left-handed view space. Returns false when it clips the near plane.
bool ProjectSphereClip(float3 C, float R, float ZNear, float P00, float P11, out float4 ClipAABB)
{
    ClipAABB = float4(-1.0, -1.0, 1.0, 1.0);

    if (C.z - R < ZNear)
    {
        return false;
    }

    // Ortho rays are parallel, so the silhouette is a plain scale of the radius.
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

    // P11 is negative (Vulkan +Y-down), which flips the y pair, so sort into min and max.
    ClipAABB = float4(
        min(xPair.x, xPair.y),
        min(yPair.x, yPair.y),
        max(xPair.x, xPair.y),
        max(yPair.x, yPair.y)
    );

    return true;
}

// Reverse-Z depth matching glm::perspective(fov, asp, Far, Near).
float SphereFrontDepth(float FrontZ, float ZNear, float ZFar)
{
    // Reverse-Z ortho is linear in view Z.
    if (IsCameraOrthographic())
    {
        return (FrontZ - ZFar) / (ZNear - ZFar);
    }

    return (ZNear * ZFar / FrontZ - ZNear) / (ZFar - ZNear);
}

// Largest axis scale of a 3x4 transform, which is how far it stretches a mesh-local error.
float TransformMaxScale(FTransform3x4 Transform)
{
    const float3 AxisX = float3(Transform.Row0.x, Transform.Row1.x, Transform.Row2.x);
    const float3 AxisY = float3(Transform.Row0.y, Transform.Row1.y, Transform.Row2.y);
    const float3 AxisZ = float3(Transform.Row0.z, Transform.Row1.z, Transform.Row2.z);
    return sqrt(max(dot(AxisX, AxisX), max(dot(AxisY, AxisY), dot(AxisZ, AxisZ))));
}

// The coarsest LOD whose projected error the view tolerates, mirrored by SelectLODForView in SceneRenderer_Extract.cpp.
uint SelectLODForView(FSurfaceDesc Desc, FCullView View, float4 SphereBounds, float MaxScale)
{
    const uint NumLODs = min(Desc.NumLODs, (uint)MESHLET_MAX_LODS);
    if (NumLODs <= 1u || View.LODErrorScale <= 0.0)
    {
        return 0u;
    }

    // Nearest point of the bounds, so a large mesh is never coarser than its closest part allows.
    const float Reach = View.HasFlag(CULL_VIEW_FLAG_ORTHOGRAPHIC_LOD)
                      ? 1.0
                      : max(length(SphereBounds.xyz - View.ViewOrigin) - SphereBounds.w, 0.0);
    const float ErrorScale = View.LODErrorScale * MaxScale;

    uint Picked = 0u;
    for (uint i = 1u; i < NumLODs; ++i)
    {
        if (Desc.LODError[i] * ErrorScale > Reach)
        {
            break;
        }
        Picked = i;
    }
    return Picked;
}

// The pick's meshlet range, x the mesh-global offset and y the count.
uint2 SelectViewMeshletRange(FSurfaceDesc Desc, FCullView View, float4 SphereBounds, float MaxScale)
{
    const uint LOD = SelectLODForView(Desc, View, SphereBounds, MaxScale);
    return uint2(Desc.LODMeshletOffset[LOD], Desc.LODMeshletCount[LOD]);
}

// Keeps the projected bounds conservative against the pyramid's own quantization.
static const float kCullSphereDilation = 1.02;

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
    P.bValid     = ProjectSphereClip(P.ViewCenter, P.ViewRadius, Scene().CullData.CullNearPlane,
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

// A box one texel wide always fits the 2x2 tap centered on it, but one two texels wide can touch three.
float HiZOcclusionMip(float4 UvAABB, float2 PyramidSize)
{
    const float2 UvExtent = UvAABB.zw - UvAABB.xy;
    const float2 Extent   = UvExtent * PyramidSize;

    float Level = ceil(log2(max(max(Extent.x, Extent.y), 1.0)));

    // One mip finer still fits the tap whenever the box's offset plus extent spans at most two texels per axis.
    const float2 FinerSize = max(floor(PyramidSize * exp2(1.0 - Level)), 1.0);
    const float2 FinerSpan = frac(UvAABB.xy * FinerSize) + UvExtent * FinerSize;
    if (Level > 0.0 && all(FinerSpan <= 2.0))
    {
        Level -= 1.0;
    }

    return Level;
}

bool IsHiZOccluded(FCullSphereProjection P)
{
    const float ZNear = Scene().CullData.CullNearPlane;
    const float ZFar  = Scene().CullData.CullFarPlane;

    // The projection already bakes +Y-down NDC, so clip maps to UV directly.
    const float4 UvAABB = saturate(P.ClipAABB * 0.5 + 0.5);

    const float Level = HiZOcclusionMip(UvAABB, float2(Scene().CullData.PyramidWidth, Scene().CullData.PyramidHeight));

    const float2 SampleUV = (UvAABB.xy + UvAABB.zw) * 0.5;

    // The min-reduction sampler returns the 2x2 minimum, which under reverse-Z is the farthest occluder.
    const float PyramidDepth = Scene().CullData.DepthPyramid.SampleLevel(Samplers::MinReduction, SampleUV, Level).x;

    const float FrontZ      = P.ViewCenter.z - P.ViewRadius;
    const float DepthSphere = SphereFrontDepth(FrontZ, ZNear, ZFar);

    // Reverse-Z, so occluded means the sphere's nearest depth sits behind that occluder.
    return DepthSphere < PyramidDepth;
}

// The previous frame's cascade depth is standard Z, so the reduction is max. bMid picks the pyramid and its transforms together.
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

    // Ortho w is 1, so clip space is already NDC.
    const float3 Ndc       = mul(CascadeVP, float4(CenterWS, 1.0)).xyz;
    const float3 NdcRadius = RadiusWS * NdcScale;

    const float2 MinNdc = Ndc.xy - NdcRadius.xy;
    const float2 MaxNdc = Ndc.xy + NdcRadius.xy;
    const float  NearZ  = Ndc.z - NdcRadius.z;

    // A sphere partly outside the cascade volume was clipped by the raster, so the pyramid cannot prove it hidden.
    if (any(MinNdc < -1.0) || any(MaxNdc > 1.0) || NearZ < 0.0)
    {
        return false;
    }

    // The ortho projection already bakes Vulkan +Y-down, so NDC maps to the tile directly.
    const float4 Tile     = Scene().CullData.CascadeHZBTile[CascadeIndex];
    const float2 LocalMin = MinNdc * 0.5 + 0.5;
    const float2 LocalMax = MaxNdc * 0.5 + 0.5;

    const float2 UvMin = Tile.xy + LocalMin * Tile.zw;
    const float2 UvMax = Tile.xy + LocalMax * Tile.zw;

    const float Level    = HiZOcclusionMip(float4(UvMin, UvMax),
                                           float2(Scene().CullData.CascadePyramidWidth, Scene().CullData.CascadePyramidHeight));
    const float MaxLevel = max((float)Scene().CullData.CascadePyramidMipCount - 1.0, 0.0);

    // Past the allocated chain one tap no longer covers the footprint, so it cannot prove anything.
    if (Level > MaxLevel)
    {
        return false;
    }

    const float2 SampleUV = (UvMin + UvMax) * 0.5;

    // Standard Z needs max reduction, the opposite of the camera's reverse-Z pyramid.
    const float PyramidDepth = Scene().CullData.CascadePyramid.SampleLevel(Samplers::MaxReduction, SampleUV, Level).x;

    // Occluded when the whole sphere sits behind the farthest depth in its footprint.
    return NearZ > PyramidDepth;
}

// CullInstances sizes buckets from this and BuildMeshletBlocks appends against it, so they must agree.
bool IsInstanceVisibleToView(FCullView View, float3 Center, float Radius, bool bNoCull, bool bCaster,
                             bool bShadowOnly, bool bFarCaster)
{
    const bool bShadowView = View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY);

    // NoCull suppresses visibility culling, not whether the instance casts.
    if (bShadowView && !bCaster)
    {
        return false;
    }
    if (View.HasFlag(CULL_VIEW_FLAG_FAR_CASCADE) && !bFarCaster)
    {
        return false;
    }

    // The mirror of the line above, so a shadow-only instance reaches shadow maps and nothing else.
    if (!bShadowView && bShadowOnly)
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
        // Cascade ortho boxes overlap, so test the camera slice swept toward the sun instead.
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

    // No camera Hi-Z here since only last frame's pyramid exists and a reject is unrecoverable. MeshletCull's late phase tests it.
    return true;
}
