#pragma once
#include "SceneGlobals.slang"
#include "VirtualShadow.slang"

// Hi-Z math here must match the depth pyramid build (min-reduction sampler, reverse-Z).

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

bool FrustumContainsSphere(FFrustum F, float3 Center, float Radius)
{
    return FrustumContainsSphere(F.Planes, Center, Radius);
}

bool FrustumIntersectsAABB(FFrustum F, float3 BoundsMin, float3 BoundsMax)
{
    const float3 Center  = (BoundsMin + BoundsMax) * 0.5;
    const float3 Extents = (BoundsMax - BoundsMin) * 0.5;

    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float4 Plane = F.Planes[i];
        float  Dist  = dot(Plane.xyz, Center) + Plane.w;
        if (Dist < -dot(abs(Plane.xyz), Extents))
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

// Thinned foliage keeps at least this share of its instances, which caps how wide the survivors grow.
static const float kMinThinningKeep = 1.0 / 16.0;

// The share of foliage kept at this squared camera distance, falling with distance squared so the count on screen stays level, 1 when not thinned.
float ThinningKeep(float DistSq, float ThinningStart)
{
    if (ThinningStart <= 0.0 || DistSq <= ThinningStart * ThinningStart)
    {
        return 1.0;
    }
    return max(ThinningStart * ThinningStart / DistSq, kMinThinningKeep);
}

// Keyed on the exact position bits, so an instance that has not moved keeps its answer every frame. Each Seed gives an independent answer.
float PositionHash(float3 Position, uint Seed = 0u)
{
    const uint3 Bits = asuint(Position);
    uint H = (Bits.x * 0x8da6b343u) ^ (Bits.y * 0xd8163841u) ^ (Bits.z * 0xcb1ab31fu) ^ Seed;
    H ^= H >> 16u;
    H *= 0x7feb352du;
    H ^= H >> 15u;
    H *= 0x846ca68bu;
    H ^= H >> 16u;
    return float(H >> 8u) * (1.0 / 16777216.0);
}

// The share of the keep threshold below it across which a thinned instance shrinks out instead of popping.
static const float kThinningFadeBand = 0.25;

// Full size for a hash at or below the band, zero above the keep threshold, so a thinned instance shrinks out instead of popping.
float ThinningFade(float Keep, float Hash)
{
    // The band closes as the share nears one, so an instance is already full size where thinning starts.
    const float Band = min(kThinningFadeBand * Keep, 1.0 - Keep);
    return saturate((Keep - Hash) / Band);
}

// World axis scale about the pivot for a kept share, widened across the ground and faded toward zero, which means dropped.
float3 ThinningScale(float Keep, float3 Pivot)
{
    if (Keep >= 1.0)
    {
        return 1.0;
    }
    const float Fade  = ThinningFade(Keep, PositionHash(Pivot));
    const float Widen = rsqrt(Keep);
    return float3(Widen, 1.0, Widen) * Fade;
}

// The share of the proxy threshold either side of it across which instances become splats one at a time instead of all at once.
static const float kProxyTransitionBand = 0.25;

// Seeded apart from thinning, whose survivors would otherwise all draw from the low end of the band.
static const uint kProxyTransitionSeed = 0x9e3779b9u;

// Whether the camera sees an instance as its proxy, each instance crossing at its own size inside the band so no row of them swaps together.
bool IsProxySized(float3 Center, float Radius, float3 Position, float TransitionPixels)
{
    const float Dither    = 2.0 * PositionHash(Position, kProxyTransitionSeed) - 1.0;
    const float Threshold = TransitionPixels * (1.0 + kProxyTransitionBand * Dither);
    return ProjectedDiameterPixels(Center, Radius) < Threshold;
}

// Group spheres nest loosely inside the instance sphere, so the reach bounds give them this much room.
static const float kClusterReachSlack = 1.25;

// The slots of a cluster surface that can hold part of this view's cut, x the first and y the last.
uint2 SelectClusterSlotRange(FSurfaceDesc Desc, FCullView View, float4 SphereBounds, float MaxScale)
{
    const uint NumSlots = min(Desc.NumLODs, (uint)MESHLET_MAX_LODS);
    if (NumSlots <= 1u || View.LODErrorScale <= 0.0)
    {
        return uint2(0u, 0u);
    }

    const bool  bOrtho     = View.HasFlag(CULL_VIEW_FLAG_ORTHOGRAPHIC_LOD);
    const float Dist       = length(SphereBounds.xyz - View.ViewOrigin);
    const float ReachNear  = bOrtho ? 1.0 : max(Dist - SphereBounds.w * kClusterReachSlack, 0.0);
    const float ReachFar   = bOrtho ? 1.0 : Dist + SphereBounds.w * kClusterReachSlack;
    const float ErrorScale = View.LODErrorScale * MaxScale * Desc.LODErrorMultiplier;

    // A slot whose every parent the view accepts draws nothing, and parents only grow with the slot.
    uint Lo = 0u;
    while (Lo + 1u < NumSlots && Desc.LODParentError[Lo] * ErrorScale <= ReachNear)
    {
        ++Lo;
    }

    // A slot whose finest error the view rejects draws nothing either, and errors only grow with the slot.
    uint Hi = NumSlots - 1u;
    while (Hi > Lo && Desc.LODError[Hi] * ErrorScale > ReachFar)
    {
        --Hi;
    }
    return uint2(Lo, Hi);
}

// A cluster mesh's ranges split on runs, MESHLET_CULL_GROUP_SIZE meshlets aligned on the mesh-global index, so each block has one run's bounds.
uint RangeBlockCount(uint2 Range, bool bRuns)
{
    if (Range.y == 0u)
    {
        return 0u;
    }
    if (!bRuns)
    {
        return (Range.y + (MESHLET_CULL_GROUP_SIZE - 1u)) / MESHLET_CULL_GROUP_SIZE;
    }
    return (Range.x + Range.y - 1u) / MESHLET_CULL_GROUP_SIZE - Range.x / MESHLET_CULL_GROUP_SIZE + 1u;
}

// Block b of a range, x its first meshlet and y its count.
uint2 RangeBlock(uint2 Range, uint Block, bool bRuns)
{
    if (!bRuns)
    {
        const uint First = Block * MESHLET_CULL_GROUP_SIZE;
        return uint2(Range.x + First, min(MESHLET_CULL_GROUP_SIZE, Range.y - First));
    }
    const uint Run   = Range.x / MESHLET_CULL_GROUP_SIZE + Block;
    const uint Begin = max(Range.x, Run * MESHLET_CULL_GROUP_SIZE);
    const uint End   = min(Range.x + Range.y, (Run + 1u) * MESHLET_CULL_GROUP_SIZE);
    return uint2(Begin, End - Begin);
}

// True when no meshlet of the run can survive the view, from the two bounds MeshBuffers appends per run after the per-meshlet ones.
bool IsMeshletRunCulled(FMeshletHeader* Header, uint Run, FCullView View, float4x4 Model, float MaxScale, float LODErrorMultiplier, bool bFrustum, bool bCut)
{
    const uint Entry = Header.MeshletCount + Run * 2u;
    const FMeshletLODBounds Lod    = loadAligned<16>(Header.LODBounds + Entry);
    const FMeshletLODBounds Extent = loadAligned<16>(Header.LODBounds + Entry + 1u);

    if (bFrustum && !FrustumContainsSphere(View.FrustumPlanes, mul(Model, float4(Extent.SelfCenter, 1.0)).xyz, Extent.SelfRadius * MaxScale))
    {
        return true;
    }
    if (!bCut)
    {
        return false;
    }

    const float ErrorScale = View.LODErrorScale * MaxScale * LODErrorMultiplier;
    if (ErrorScale <= 0.0)
    {
        return Lod.SelfError > 0.0;
    }

    const bool bOrtho = View.HasFlag(CULL_VIEW_FLAG_ORTHOGRAPHIC_LOD);
    // Even the finest group rejected from the run's far side means every meshlet's own group is rejected.
    const float3 SelfCenter = mul(Model, float4(Lod.SelfCenter, 1.0)).xyz;
    const float  FarReach   = bOrtho ? 1.0 : length(SelfCenter - View.ViewOrigin) + Lod.SelfRadius * MaxScale;
    if (Lod.SelfError * ErrorScale > FarReach)
    {
        return true;
    }

    if (Lod.ParentError >= MESHLET_LOD_ERROR_INFINITE)
    {
        return false;
    }
    // Even the coarsest parent accepted from the run's near side means every meshlet's parent draws instead.
    const float3 ParentCenter = mul(Model, float4(Lod.ParentCenter, 1.0)).xyz;
    const float  NearReach    = bOrtho ? 1.0 : max(length(ParentCenter - View.ViewOrigin) - Lod.ParentRadius * MaxScale, 0.0);
    return Lod.ParentError * ErrorScale <= NearReach;
}

// The pick's meshlet range, x the mesh-global offset and y the count, and the most of it one cut can draw. A cluster surface's slots are contiguous, so a slot span is one range.
uint2 SelectViewMeshletRange(FSurfaceDesc Desc, FCullView View, float4 SphereBounds, float MaxScale, out uint DrawBound)
{
    const uint2 Slots = SelectClusterSlotRange(Desc, View, SphereBounds, MaxScale);
    const uint  Begin = Desc.LODMeshletOffset[Slots.x];
    const uint  End   = Desc.LODMeshletOffset[Slots.y] + Desc.LODMeshletCount[Slots.y];
    const uint  Count = End > Begin ? End - Begin : 0u;

    // The cut gathers around the slot the instance center picks and spreads a slot either way, which is what the list is reserved for; the survivors the readback reports grow it from there.
    const float Dist = View.HasFlag(CULL_VIEW_FLAG_ORTHOGRAPHIC_LOD) ? 1.0 : length(SphereBounds.xyz - View.ViewOrigin);
    const float ES   = View.LODErrorScale * MaxScale * Desc.LODErrorMultiplier;
    uint Mid = Slots.x;
    for (uint s = Slots.x + 1u; s <= Slots.y; ++s)
    {
        if (Desc.LODError[s] * ES > Dist)
        {
            break;
        }
        Mid = s;
    }
    const uint First = Mid > Slots.x ? Mid - 1u : Slots.x;
    const uint Last  = min(Mid + 1u, Slots.y);
    uint Expected = MESHLET_CULL_GROUP_SIZE;
    for (uint s = First; s <= Last; ++s)
    {
        Expected += Desc.LODMeshletCount[s];
    }
    DrawBound = min(Count, Expected);
    return uint2(Begin, Count);
}

// Diameter of a world sphere in pixels of the cull camera, unbounded when the camera is inside it.
float ProjectedDiameterPixels(float3 Center, float Radius)
{
    const float4x4 Proj   = GetCullCameraProjection();
    const float    Height = float(Scene().ScreenSize.y);
    if (IsCameraOrthographic())
    {
        return Radius * abs(Proj[1][1]) * Height;
    }

    // A sphere the camera is inside fills the screen, so it never reads as too small to draw.
    const float ViewZ = mul(GetCullCameraView(), float4(Center, 1.0)).z;
    if (ViewZ <= Radius)
    {
        return 3.0e38;
    }
    return Radius * abs(Proj[1][1]) * Height / ViewZ;
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

// A skinned instance always moves. Anything else counts only while the sun's cache could hold it but has not, which is a body that just moved.
bool IsMover(bool bSkinned, bool bShadowCacheable, bool bShadowCached)
{
    return bSkinned || (GetSceneRoot().ShadowCacheBits.Count > 0u && bShadowCacheable && !bShadowCached);
}

// A shadow level splits its casters between its static and dynamic views, so each instance lands in exactly one.
bool IsShadowCacheSplitRejected(FCullView View, bool bShadowCached, bool bMover)
{
    return (View.HasFlag(CULL_VIEW_FLAG_SHADOW_CACHE_STATIC) && !bShadowCached)
        || (View.HasFlag(CULL_VIEW_FLAG_SHADOW_CACHE_DYNAMIC) && bShadowCached)
        || (View.HasFlag(CULL_VIEW_FLAG_MOVERS_ONLY) && !bMover);
}

// CullInstances sizes buckets from this and BuildMeshletBlocks appends against it, so they must agree.
bool IsInstanceVisibleToView(FCullView View, float3 Center, float Radius, bool bNoCull, bool bCaster,
                             bool bShadowOnly, bool bFoliage, bool bCustomDepth, uint Layers, bool bShadowCached, bool bMover)
{
    if ((View.LayerMask & Layers) == 0u)
    {
        return false;
    }

    if (IsShadowCacheSplitRejected(View, bShadowCached, bMover))
    {
        return false;
    }

    if (View.HasFlag(CULL_VIEW_FLAG_CUSTOM_DEPTH_ONLY) && !bCustomDepth)
    {
        return false;
    }

    if (View.HasFlag(CULL_VIEW_FLAG_NO_FOLIAGE) && bFoliage)
    {
        return false;
    }

    const bool bShadowView = View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY);

    // NoCull suppresses visibility culling, not whether the instance casts.
    if (bShadowView && !bCaster)
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

    // A shadow level keeps what lands in last frame's requested pages grown by two, and the fresh pages it misses draw again next frame.
    if (View.HasFlag(CULL_VIEW_FLAG_VIRTUAL_SHADOW) && IsVirtualShadowPageCulled(View.ShadowLevel, VSM_MASK_HOT, Center, Radius))
    {
        return false;
    }

    // No camera Hi-Z here since only last frame's pyramid exists and a reject is unrecoverable. MeshletCull's late phase tests it.
    return true;
}
