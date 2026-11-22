#pragma once
#include "SceneGlobals.slang"

// Page addressing, level choice and the mask test shared by the page passes, the cull, the raster and the lighting.

FVirtualShadowLevel VirtualShadowLevel(uint Level) { return GetSceneRoot().VirtualShadowLevels.GetAligned(Level); }

// A global page sits at itself modulo the level width, so a scrolling level keeps every page it still covers.
uint VirtualShadowPageIndex(uint Level, int2 GlobalPage)
{
    const uint2 Wrapped = uint2(GlobalPage & int2(VSM_LEVEL_PAGES - 1u, VSM_LEVEL_PAGES - 1u));
    return Level * VSM_PAGES_PER_LEVEL + Wrapped.y * VSM_LEVEL_PAGES + Wrapped.x;
}

uint VirtualShadowPageTag(int2 GlobalPage)
{
    const int2 Wrap = GlobalPage >> int(VSM_LEVEL_PAGES_SHIFT);
    return (uint(Wrap.x) & 0xFFFFu) | (uint(Wrap.y) << 16u);
}

// The continuous level for a distance, whose ceiling is the level whose texel matches the pixel's footprint.
float VirtualShadowLevelPosition(float Distance)
{
    return log2(max(Distance * LightData().SunShadow.FootprintScale, 1.0e-8));
}

int VirtualShadowLevelForDistance(float Distance)
{
    return (int)ceil(VirtualShadowLevelPosition(Distance));
}

// The outer share of each level's distance range that fades into the next coarser level, so the step in texel size has no edge.
static const float kVirtualShadowBlendBand = 0.3;

// How far into the fade toward the next level a distance is, 0 outside the band and 1 at the boundary.
float VirtualShadowLevelBlend(float Distance)
{
    const float Position = VirtualShadowLevelPosition(Distance);
    const float Within   = Position - (ceil(Position) - 1.0);
    return saturate((Within - (1.0 - kVirtualShadowBlendBand)) / kVirtualShadowBlendBand);
}

uint ClampVirtualShadowLevel(int Level)
{
    return (uint)clamp(Level, (int)LightData().SunShadow.FirstLevel, (int)LightData().SunShadow.LastLevel);
}

// Virtual texel and depth of a world position in one level, false outside the level's box.
bool ProjectToVirtualShadow(FVirtualShadowLevel Lv, float3 WorldPos, out float2 Texel, out float Depth)
{
    const float3 Ndc = mul(Lv.ViewProjection, float4(WorldPos, 1.0)).xyz;
    Texel = (Ndc.xy * 0.5 + 0.5) * float(VSM_VIRTUAL_SIZE);
    Depth = Ndc.z;
    return all(Texel >= 0.0) && all(Texel < float(VSM_VIRTUAL_SIZE)) && Depth >= 0.0 && Depth <= 1.0;
}

uint2 VirtualShadowPhysicalPageOrigin(uint Physical, bool bDynamicLayer)
{
    const uint Row = Physical / VSM_POOL_PAGES_X + (bDynamicLayer ? GetSceneRoot().VirtualShadowPoolRows : 0u);
    return uint2(Physical % VSM_POOL_PAGES_X, Row) * VSM_PAGE_SIZE;
}

// The resident physical page under a virtual page, false when none is. Requested pages hold this frame's movers in the dynamic layer.
bool FindVirtualShadowPage(uint Level, FVirtualShadowLevel Lv, int2 VirtualPage, out uint2 PhysicalOrigin)
{
    PhysicalOrigin = uint2(0u, 0u);
    if (any(VirtualPage < 0) || any(VirtualPage >= int(VSM_LEVEL_PAGES)))
    {
        return false;
    }
    const int2 Global = Lv.PageOrigin + VirtualPage;
    const FVirtualShadowPage Page = GetSceneRoot().VirtualShadowPages.Get(VirtualShadowPageIndex(Level, Global));
    if ((Page.Entry & VSM_PAGE_ALLOCATED) == 0u || Page.Tag != VirtualShadowPageTag(Global))
    {
        return false;
    }
    PhysicalOrigin = VirtualShadowPhysicalPageOrigin(Page.Entry & VSM_PAGE_PHYSICAL_MASK, (Page.Entry & VSM_PAGE_REQUESTED) != 0u);
    return true;
}

float LoadVirtualShadowDepth(uint2 Physical)
{
    return asfloat(GetSceneRoot().VirtualShadowPool.Load(int2(Physical)));
}

uint VirtualShadowMaskMipOffset(uint Mip)
{
    uint Offset = 0u;
    for (uint m = 0u; m < Mip; ++m)
    {
        const uint Side = VSM_LEVEL_PAGES >> m;
        Offset += Side * Side;
    }
    return Offset;
}

// True when no page under the sphere's footprint in this level carries Bit, read from the mask mip where the footprint spans at most two cells.
bool IsVirtualShadowPageCulled(uint Level, uint Bit, float3 Center, float Radius)
{
    const GPUSpan<uint> Masks = GetSceneRoot().VirtualShadowMasks;
    if (Masks.IsEmpty())
    {
        return false;
    }

    const FVirtualShadowLevel Lv = VirtualShadowLevel(Level);
    const float4x4 VP  = Lv.ViewProjection;
    const float3   Ndc = mul(VP, float4(Center, 1.0)).xyz;
    // Orthographic, so each row's length is the NDC change per world unit along that axis.
    const float2   R   = Radius * float2(length(VP[0].xyz), length(VP[1].xyz));
    const float2 MinTexel = ((Ndc.xy - R) * 0.5 + 0.5) * float(VSM_VIRTUAL_SIZE);
    const float2 MaxTexel = ((Ndc.xy + R) * 0.5 + 0.5) * float(VSM_VIRTUAL_SIZE);
    if (any(MaxTexel < 0.0) || any(MinTexel >= float(VSM_VIRTUAL_SIZE)))
    {
        return true;
    }

    const int2 Last = int2(VSM_LEVEL_PAGES - 1u, VSM_LEVEL_PAGES - 1u);
    const int2 Lo   = clamp(int2(floor(MinTexel)) >> int(VSM_PAGE_SHIFT), int2(0, 0), Last) + Lv.PageOrigin;
    const int2 Hi   = clamp(int2(floor(MaxTexel)) >> int(VSM_PAGE_SHIFT), int2(0, 0), Last) + Lv.PageOrigin;
    const int2 Span = Hi - Lo;
    uint Mip = 0u;
    while (Mip + 1u < VSM_MASK_MIPS && ((Span.x >> int(Mip)) > 1 || (Span.y >> int(Mip)) > 1))
    {
        ++Mip;
    }

    const int  Side   = int(VSM_LEVEL_PAGES >> Mip);
    const uint Base   = Level * VSM_MASK_CELLS_PER_LEVEL + VirtualShadowMaskMipOffset(Mip);
    const int2 CellLo = Lo >> int(Mip);
    const int2 CellHi = Hi >> int(Mip);
    for (int y = CellLo.y; y <= CellHi.y; ++y)
    {
        for (int x = CellLo.x; x <= CellHi.x; ++x)
        {
            const uint2 Cell = uint2(int2(x, y) & int2(Side - 1, Side - 1));
            if ((Masks.Get(Base + Cell.y * uint(Side) + Cell.x) & Bit) != 0u)
            {
                return false;
            }
        }
    }
    return true;
}

// The deferred shadow phase's reject, against the pages this frame's page pass built for the view's layer.
bool IsShadowLevelCasterCulled(FCullView View, float3 Center, float Radius)
{
    const uint Bit = View.HasFlag(CULL_VIEW_FLAG_SHADOW_CACHE_STATIC) ? VSM_MASK_STATIC : VSM_MASK_REQUESTED;
    return IsVirtualShadowPageCulled(View.ShadowLevel, Bit, Center, Radius);
}

uint VirtualShadowPageEntry(uint Level, int2 GlobalPage)
{
    return GetSceneRoot().VirtualShadowPages.At(VirtualShadowPageIndex(Level, GlobalPage)).Entry;
}

bool IsVirtualShadowPageDrawn(uint PageEntry, bool bStatic)
{
    return (PageEntry & (bStatic ? VSM_PAGE_RENDER_STATIC : VSM_PAGE_REQUESTED)) != 0u;
}

// The raster's write of one virtual texel through its page's entry, which lands only in pages the layer draws this frame.
void WriteVirtualShadowPageTexel(uint PageEntry, bool bStatic, uint2 Texel, float Depth)
{
    if (!IsVirtualShadowPageDrawn(PageEntry, bStatic) || Depth < 0.0 || Depth > 1.0)
    {
        return;
    }

    const uint2 Physical = VirtualShadowPhysicalPageOrigin(PageEntry & VSM_PAGE_PHYSICAL_MASK, !bStatic) + (Texel & (VSM_PAGE_SIZE - 1u));
    GetSceneRoot().VirtualShadowPoolRW.InterlockedMin(Physical, asuint(Depth));
}

uint VirtualShadowFragmentEntry(uint Level, float4 Position)
{
    return VirtualShadowPageEntry(Level, VirtualShadowLevel(Level).PageOrigin + int2(uint2(Position.xy) >> VSM_PAGE_SHIFT));
}

void WriteVirtualShadowLevelDepth(uint Level, bool bStatic, float4 Position)
{
    WriteVirtualShadowPageTexel(VirtualShadowFragmentEntry(Level, Position), bStatic, uint2(Position.xy), Position.z);
}

// A mesh caster's write, whose cull view names its level and layer.
void WriteVirtualShadowDepth(uint CullViewIndex, float4 Position)
{
    const FCullView View = CullViews().GetAligned(CullViewIndex);
    if (View.HasFlag(CULL_VIEW_FLAG_VIRTUAL_SHADOW))
    {
        WriteVirtualShadowLevelDepth(View.ShadowLevel, View.HasFlag(CULL_VIEW_FLAG_SHADOW_CACHE_STATIC), Position);
    }
}

// True for a virtual view's fragment in a page its layer leaves alone this frame, and pages hold whole quads, so derivatives survive the skip.
bool IsVirtualShadowFragmentSkipped(uint CullViewIndex, float4 Position)
{
    const FCullView View = CullViews().GetAligned(CullViewIndex);
    return View.HasFlag(CULL_VIEW_FLAG_VIRTUAL_SHADOW)
        && !IsVirtualShadowPageDrawn(VirtualShadowFragmentEntry(View.ShadowLevel, Position), View.HasFlag(CULL_VIEW_FLAG_SHADOW_CACHE_STATIC));
}

// Coarser levels a lookup climbs through when its own level holds no page there.
static const uint kVirtualShadowFallbackLevels = 3u;

// A single compare at the finest resident level, for fog, particles and other points with no surface to filter across.
float SampleVirtualShadowPoint(float3 WorldPos)
{
    const int Wanted = VirtualShadowLevelForDistance(length(WorldPos - GetCameraPosition()));
    const uint Last  = LightData().SunShadow.LastLevel;
    if (Wanted > (int)Last)
    {
        return 1.0;
    }

    const uint First = ClampVirtualShadowLevel(Wanted);
    [loop]
    for (uint Level = First; Level <= min(First + kVirtualShadowFallbackLevels, Last); ++Level)
    {
        const FVirtualShadowLevel Lv = VirtualShadowLevel(Level);
        float2 Texel;
        float  Depth;
        uint2  Origin;
        if (!ProjectToVirtualShadow(Lv, WorldPos, Texel, Depth) || !FindVirtualShadowPage(Level, Lv, int2(Texel) >> int(VSM_PAGE_SHIFT), Origin))
        {
            continue;
        }
        const float Occluder = LoadVirtualShadowDepth(Origin + (uint2(Texel) & (VSM_PAGE_SIZE - 1u)));
        const float Bias     = 2.0 * Lv.TexelWorld / max(Lv.DepthRange, 1.0e-4);
        return Depth - Bias > Occluder ? 0.0 : 1.0;
    }
    return 1.0;
}

// World distance from a point to the nearest occluder toward the sun, 0 where nothing is recorded.
float VirtualShadowOccluderDistance(float3 WorldPos)
{
    const int Wanted = VirtualShadowLevelForDistance(length(WorldPos - GetCameraPosition()));
    const uint Last  = LightData().SunShadow.LastLevel;
    if (Wanted > (int)Last)
    {
        return 0.0;
    }

    const uint First = ClampVirtualShadowLevel(Wanted);
    [loop]
    for (uint Level = First; Level <= min(First + kVirtualShadowFallbackLevels, Last); ++Level)
    {
        const FVirtualShadowLevel Lv = VirtualShadowLevel(Level);
        float2 Texel;
        float  Depth;
        uint2  Origin;
        if (!ProjectToVirtualShadow(Lv, WorldPos, Texel, Depth) || !FindVirtualShadowPage(Level, Lv, int2(Texel) >> int(VSM_PAGE_SHIFT), Origin))
        {
            continue;
        }
        const float Occluder = LoadVirtualShadowDepth(Origin + (uint2(Texel) & (VSM_PAGE_SIZE - 1u)));
        return max(Depth - Occluder, 0.0) * Lv.DepthRange;
    }
    return 0.0;
}
