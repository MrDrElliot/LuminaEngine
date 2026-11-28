#pragma once
#include "SceneGlobals.slang"

// The terrain a shader invocation is culling, drawing or shading, set by its entry point before any terrain read.
static uint GTerrainIndex = 0u;

FTerrainRecord* TerrainArgs() { return GetSceneRoot().Terrains.At(min(GTerrainIndex, max(GetSceneRoot().Terrains.Count, 1u) - 1u)); }

FTerrainRenderParams GetTerrainParams() { return TerrainArgs().Params; }

// Mirrors TerrainRenderTypes.h. Packed, the depth takes the top four bits and the column and row fourteen each.
struct FTerrainNode
{
    uint Depth;
    int2 Coord;
};

uint PackTerrainNode(uint Depth, int2 Coord)
{
    return (Depth << 28u) | ((uint)Coord.y << 14u) | (uint)Coord.x;
}

FTerrainNode UnpackTerrainNode(uint Packed)
{
    FTerrainNode Node;
    Node.Depth = Packed >> 28u;
    Node.Coord = int2(Packed & 0x3FFFu, (Packed >> 14u) & 0x3FFFu);
    return Node;
}

// Level 0 is the leaves, whose quads step one heightmap sample, and each level up doubles the step.
uint TerrainNodeLevel(FTerrainRenderParams Params, uint Depth)
{
    return (uint)Params.TreeDepth - Depth;
}

int TerrainNodeSpan(uint Level)
{
    return int(TERRAIN_MESHLET_QUADS << Level);
}

uint TerrainNodeIndex(FTerrainNode Node)
{
    return ((1u << (2u * Node.Depth)) - 1u) / 3u + (uint)Node.Coord.y * (1u << Node.Depth) + (uint)Node.Coord.x;
}

// False for a node past the heightmap's last sample, which holds no surface.
bool TerrainNodeBounds(FTerrainRecord* Record, FTerrainNode Node, out float3 BoundsMin, out float3 BoundsMax)
{
    BoundsMin = float3(0.0, 0.0, 0.0);
    BoundsMax = float3(0.0, 0.0, 0.0);
    // Bounds exist down to the page nodes, and a deeper node takes its page's height range over its own footprint.
    const uint PageDepth = (uint)max(Record.Params.PageDepth, 0);
    FTerrainNode Owner = Node;
    if (Node.Depth > PageDepth)
    {
        Owner.Depth = PageDepth;
        Owner.Coord = Node.Coord >> int(Node.Depth - PageDepth);
    }
    const uint Index = TerrainNodeIndex(Owner);
    if (!Record.NodeBounds.Contains(Index))
    {
        return false;
    }
    const float2 Range = Record.NodeBounds.Get(Index);
    if (Range.x > Range.y)
    {
        return false;
    }

    const FTerrainRenderParams Params = Record.Params;
    const int  Span   = TerrainNodeSpan(TerrainNodeLevel(Params, Node.Depth));
    const int2 Origin = Node.Coord * Span;
    if (any(Origin >= int2(Params.Resolution - 1, Params.Resolution - 1)))
    {
        return false;
    }
    const int2 End    = min(Origin + Span, int2(Params.Resolution - 1, Params.Resolution - 1));
    const float Stride = Params.SampleSpacing;

    // A negative MaxHeight swaps which end of the range is low.
    const float Low  = min(Range.x * Params.MaxHeight, Range.y * Params.MaxHeight);
    const float High = max(Range.x * Params.MaxHeight, Range.y * Params.MaxHeight);

    // Y slack, since matrix rounding can push a vertex just outside the stored range and cull a silhouette.
    const float DilateXZ = Stride * 0.5;
    const float DilateY  = max(0.05, abs(Params.MaxHeight) * 0.01);

    BoundsMin = float3(Params.OriginXZ.x + float(Origin.x) * Stride - DilateXZ, Params.WorldOriginY.x + Low - DilateY,
                       Params.OriginXZ.y + float(Origin.y) * Stride - DilateXZ);
    BoundsMax = float3(Params.OriginXZ.x + float(End.x) * Stride + DilateXZ, Params.WorldOriginY.x + High + DilateY,
                       Params.OriginXZ.y + float(End.y) * Stride + DilateXZ);
    return true;
}

// Where a sample's heights, normals and weights live, the finest resident page at the asked mip or coarser.
struct FTerrainPage
{
    uint Slot;
    uint Mip;
    int2 Origin;
    bool bValid;
    // The mip the lookup asked for, which a coarser page may have answered.
    uint Asked;
};

uint TerrainPageMaxMip(FTerrainRecord* Record)
{
    return (uint)max(Record.Params.PageDepth, 0);
}

// The highest ground under the page node holding Sample at Mip, which no height read there exceeds, or very low where the map has none.
float TerrainMaxHeightAt(FTerrainRecord* Record, float2 Sample, uint Mip)
{
    const uint  MaxMip = TerrainPageMaxMip(Record);
    const uint  UseMip = min(Mip, MaxMip);
    const uint  Depth  = MaxMip - UseMip;
    const int2  Coord  = clamp(int2(floor(Sample)) >> int(TERRAIN_PAGE_QUADS_LOG2 + UseMip), int2(0, 0), int2((1 << Depth) - 1, (1 << Depth) - 1));
    const uint  Index  = ((1u << (2u * Depth)) - 1u) / 3u + (uint)Coord.y * (1u << Depth) + (uint)Coord.x;
    if (!Record.NodeBounds.Contains(Index))
    {
        return 3.0e38;
    }
    const float2 Range = Record.NodeBounds.Get(Index);
    if (Range.x > Range.y)
    {
        return -3.0e38;
    }
    return Record.Params.WorldOriginY.x + max(Range.x * Record.Params.MaxHeight, Range.y * Record.Params.MaxHeight);
}

// Every coarser page backs a missing finer one, down to the root page, which is always resident.
FTerrainPage TerrainFindPage(FTerrainRecord* Record, int2 Sample, uint Mip)
{
    FTerrainPage Page = (FTerrainPage)0;
    Page.Asked = Mip;
    const uint MaxMip = TerrainPageMaxMip(Record);
    for (uint PageMip = min(Mip, MaxMip); PageMip <= MaxMip; ++PageMip)
    {
        const uint Depth = MaxMip - PageMip;
        const int  Side  = 1 << Depth;
        // Arithmetic shifts rather than a division by the span, which a GPU spends twenty instructions on.
        const int2 Coord = clamp(Sample >> int(TERRAIN_PAGE_QUADS_LOG2 + PageMip), int2(0, 0), int2(Side - 1, Side - 1));
        const uint Index = ((1u << (2u * Depth)) - 1u) / 3u + uint(Coord.y * Side + Coord.x);
        const uint Slot  = Record.PageTable.Contains(Index) ? Record.PageTable.Get(Index) : TERRAIN_PAGE_NONE;
        if (Slot != TERRAIN_PAGE_NONE)
        {
            Page.Slot   = Slot;
            Page.Mip    = PageMip;
            Page.Origin = Coord << int(TERRAIN_PAGE_QUADS_LOG2 + PageMip);
            Page.bValid = true;
            return Page;
        }
    }
    return Page;
}

// Skips the table walk while samples stay inside the page the last one found, which a node's vertices and a shadow march nearly always do.
FTerrainPage TerrainFindPageCached(FTerrainRecord* Record, int2 Sample, uint Mip, inout FTerrainPage Cache)
{
    // A coarser page standing in for a missing finer one answers the same ask anywhere inside it, at worst a mip coarser than a neighbor found.
    const int Span = int(TERRAIN_PAGE_QUADS) << Cache.Mip;
    const bool bHolds = Cache.bValid && Cache.Asked == Mip && all(Sample >= Cache.Origin) && all(Sample < Cache.Origin + Span);
    if (!bHolds)
    {
        Cache = TerrainFindPage(Record, Sample, Mip);
    }
    return Cache;
}

// In atlas texels, from a position in samples of the finest grid, where integers are sample centers.
float2 TerrainPageTexel(FTerrainRecord* Record, FTerrainPage Page, float2 SamplePos)
{
    const float2 Slot = float2(float(Page.Slot & (TERRAIN_PAGE_ATLAS_SLOTS_SIDE - 1u)), float(Page.Slot >> TERRAIN_PAGE_ATLAS_SLOTS_LOG2)) * float(TERRAIN_PAGE_TEXELS);
    return Slot + (SamplePos - float2(Page.Origin)) * exp2(-float(Page.Mip)) + float(TERRAIN_PAGE_GUTTER);
}

float2 TerrainPageUV(FTerrainRecord* Record, FTerrainPage Page, float2 SamplePos)
{
    return (TerrainPageTexel(Record, Page, SamplePos) + 0.5) * (1.0 / float(TERRAIN_PAGE_ATLAS_SLOTS_SIDE * TERRAIN_PAGE_TEXELS));
}

bool TerrainPageOnGrid(FTerrainPage Page, int2 Sample)
{
    return all(((Sample - Page.Origin) & int((1u << Page.Mip) - 1u)) == 0);
}

// The pages pack XZ, so Y comes back from unit length.
float3 TerrainDecodeNormal(float2 Packed)
{
    const float2 XZ = Packed * 2.0 - 1.0;
    return float3(XZ.x, sqrt(saturate(1.0 - dot(XZ, XZ))), XZ.y);
}

float TerrainPageHeight(FTerrainRecord* Record, FTerrainPage Page, float2 SamplePos)
{
    return Record.HeightAtlas.SampleLevel(Samplers::LinearClamp, TerrainPageUV(Record, Page, SamplePos), 0).r * Record.Params.MaxHeight;
}

float3 TerrainPageNormal(FTerrainRecord* Record, FTerrainPage Page, float2 SamplePos)
{
    return TerrainDecodeNormal(Record.NormalAtlas.SampleLevel(Samplers::LinearClamp, TerrainPageUV(Record, Page, SamplePos), 0).rg);
}

float4 TerrainPageWeights(FTerrainRecord* Record, FTerrainPage Page, float2 SamplePos)
{
    return Record.WeightAtlas.SampleLevel(Samplers::LinearClamp, TerrainPageUV(Record, Page, SamplePos), 0);
}

// World height above the terrain's base, exact for a sample on the grid of a resident page at Mip.
float TerrainHeightAtSample(FTerrainRecord* Record, int2 Sample, uint Mip, inout FTerrainPage Cache)
{
    const FTerrainPage Page = TerrainFindPageCached(Record, Sample, Mip, Cache);
    if (!Page.bValid)
    {
        return 0.0;
    }
    if (TerrainPageOnGrid(Page, Sample))
    {
        return Record.HeightAtlas.Load(int2(TerrainPageTexel(Record, Page, float2(Sample)))).r * Record.Params.MaxHeight;
    }
    return TerrainPageHeight(Record, Page, float2(Sample));
}

float3 TerrainNormalAtSample(FTerrainRecord* Record, int2 Sample, uint Mip, inout FTerrainPage Cache)
{
    const FTerrainPage Page = TerrainFindPageCached(Record, Sample, Mip, Cache);
    if (!Page.bValid)
    {
        return float3(0.0, 1.0, 0.0);
    }
    if (TerrainPageOnGrid(Page, Sample))
    {
        return TerrainDecodeNormal(Record.NormalAtlas.Load(int2(TerrainPageTexel(Record, Page, float2(Sample)))).rg);
    }
    return TerrainPageNormal(Record, Page, float2(Sample));
}

float TerrainHeightAt(FTerrainRecord* Record, float2 SamplePos, uint Mip, inout FTerrainPage Cache)
{
    const FTerrainPage Page = TerrainFindPageCached(Record, int2(floor(SamplePos)), Mip, Cache);
    return Page.bValid ? TerrainPageHeight(Record, Page, SamplePos) : 0.0;
}

float TerrainHeightAt(FTerrainRecord* Record, float2 SamplePos, uint Mip)
{
    FTerrainPage Cache = (FTerrainPage)0;
    return TerrainHeightAt(Record, SamplePos, Mip, Cache);
}

// The two mips around a fractional one, found once so normals and weights share the lookups. Far stays unused when a coarser page stood in for the near one.
struct FTerrainMipPair
{
    FTerrainPage Near;
    FTerrainPage Far;
    float        Blend;
};

FTerrainMipPair TerrainFindMipPair(FTerrainRecord* Record, float2 SamplePos, float Mip)
{
    const float Level = max(Mip, 0.0);
    const uint  Fine  = (uint)Level;
    const int2  Sample = int2(floor(SamplePos));

    FTerrainMipPair Pair = (FTerrainMipPair)0;
    Pair.Near = TerrainFindPage(Record, Sample, Fine);
    if (Pair.Near.bValid && Pair.Near.Mip == Fine && Fine < TerrainPageMaxMip(Record) && frac(Level) > 0.0)
    {
        Pair.Far   = TerrainFindPage(Record, Sample, Fine + 1u);
        Pair.Blend = frac(Level);
    }
    return Pair;
}

float3 TerrainPairNormal(FTerrainRecord* Record, FTerrainMipPair Pair, float2 SamplePos)
{
    if (!Pair.Near.bValid)
    {
        return float3(0.0, 1.0, 0.0);
    }
    const float3 Normal = TerrainPageNormal(Record, Pair.Near, SamplePos);
    return Pair.Blend > 0.0 ? normalize(lerp(Normal, TerrainPageNormal(Record, Pair.Far, SamplePos), Pair.Blend)) : Normal;
}

float4 TerrainPairWeights(FTerrainRecord* Record, FTerrainMipPair Pair, float2 SamplePos)
{
    if (!Pair.Near.bValid)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float4 Weights = TerrainPageWeights(Record, Pair.Near, SamplePos);
    return Pair.Blend > 0.0 ? lerp(Weights, TerrainPageWeights(Record, Pair.Far, SamplePos), Pair.Blend) : Weights;
}

float3 TerrainNormalAt(FTerrainRecord* Record, float2 SamplePos, float Mip)
{
    return TerrainPairNormal(Record, TerrainFindMipPair(Record, SamplePos, Mip), SamplePos);
}

float4 TerrainWeightsAt(FTerrainRecord* Record, float2 SamplePos, float Mip)
{
    return TerrainPairWeights(Record, TerrainFindMipPair(Record, SamplePos, Mip), SamplePos);
}

// Positions in samples of the finest grid, from world XZ.
float2 TerrainSamplePos(FTerrainRecord* Record, float2 WorldXZ)
{
    return (WorldXZ - Record.Params.OriginXZ) / Record.Params.SampleSpacing;
}

bool TerrainCovers(FTerrainRecord* Record, float2 WorldXZ)
{
    const float2 Pos = TerrainSamplePos(Record, WorldXZ);
    return all(Pos >= 0.0) && all(Pos <= float(Record.Params.Resolution - 1));
}

// The mip whose texels match a footprint in world units, which is what a far reader can expect to find resident.
float TerrainFootprintMip(FTerrainRecord* Record, float FootprintWorld)
{
    return max(log2(max(FootprintWorld / Record.Params.SampleSpacing, 1.0)), 0.0);
}
