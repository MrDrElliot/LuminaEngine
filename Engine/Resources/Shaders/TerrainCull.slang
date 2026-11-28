// No Hi-Z here, since the inter-phase pyramid lacks full BasePass depth and pops whole nodes out.

#include "Includes/AppendBuffer.slang"
#include "Includes/Culling.slang"
#include "Includes/TerrainCommon.slang"

#define LOCAL_SIZE_X 64

// Pointers first so each 8-byte address stays 8-aligned.
struct FTerrainCullPushConstants
{
    GPUSpan<uint>   NodesIn;
    GPUSpan<uint>   NodesOut;
    // Four words per depth, its dispatch group count, two ones, then its node count.
    GPUSpan<uint>   LevelArgs;
    // Lumina::FTerrainDrawArgs as words, the indexed InstanceCount at 1 and the mesh task group count at 5.
    GPUSpan<uint>   TerrainIndirect;
    uint            TerrainIndex;
    uint            Depth;
    uint            _Pad0;
    uint            _Pad1;
};

FTerrainCullPushConstants* Pass() { return GetPassArgs<FTerrainCullPushConstants>(); }

static const uint kTerrainDrawInstanceWord  = 1u;
static const uint kTerrainDrawMeshGroupWord = 5u;
static const uint kLevelArgsWords           = 4u;
static const uint kLevelCountWord           = 3u;

// Widest a terrain quad may look before its node splits, since the normal map carries the detail between vertices.
static const float kTerrainQuadPixels = 6.0;

bool TerrainNodeNeedsFiner(FTerrainRenderParams Params, uint Level, float3 BoundsMin, float3 BoundsMax)
{
    if (Level == 0u)
    {
        return false;
    }
    // The Vulkan projection flips Y, so its [1][1] is negative and only its magnitude scales.
    const float PixelsPerUnit = abs(GetCullCameraProjection()[1][1]) * 0.5 * float(GetScreenSize().y);
    const float QuadWorld     = Params.SampleSpacing * float(1u << Level);
    if (IsCameraOrthographic())
    {
        return QuadWorld * PixelsPerUnit > kTerrainQuadPixels;
    }
    const float3 Camera   = Scene().CullData.CullCameraPosition.xyz;
    const float  Distance = length(Camera - clamp(Camera, BoundsMin, BoundsMax));
    return QuadWorld * PixelsPerUnit / max(Distance, 1e-3) > kTerrainQuadPixels;
}

// Children draw at the next level down, whose page must be resident, or their vertices would read a coarser one and crack against neighbors.
bool TerrainChildPageResident(FTerrainRecord* Record, FTerrainNode Node, uint Level)
{
    if (Level == 0u)
    {
        return false;
    }
    const uint ChildMip = min(Level - 1u, (uint)max(Record.Params.PageDepth, 0));
    const FTerrainPage Page = TerrainFindPage(Record, Node.Coord * TerrainNodeSpan(Level), ChildMip);
    return Page.bValid && Page.Mip == ChildMip;
}

bool TerrainNodeSplits(FTerrainRecord* Record, FTerrainNode Node, uint Level, float3 BoundsMin, float3 BoundsMax)
{
    return TerrainNodeNeedsFiner(Record.Params, Level, BoundsMin, BoundsMax) && TerrainChildPageResident(Record, Node, Level);
}

// The level drawn over Sample, found by repeating the split test from the root, and never finer than MaxDepth since a finer neighbor snaps to us itself.
uint TerrainCutLevelAt(FTerrainRecord* Record, int2 Sample, uint MaxDepth)
{
    const FTerrainRenderParams Params = Record.Params;
    for (uint Depth = 0u; Depth < MaxDepth; ++Depth)
    {
        const uint Level = TerrainNodeLevel(Params, Depth);
        FTerrainNode Node;
        Node.Depth = Depth;
        Node.Coord = Sample >> int(TERRAIN_MESHLET_QUADS_LOG2 + Level);

        float3 BoundsMin, BoundsMax;
        if (!TerrainNodeBounds(Record, Node, BoundsMin, BoundsMax) || !TerrainNodeSplits(Record, Node, Level, BoundsMin, BoundsMax))
        {
            return Level;
        }
    }
    return TerrainNodeLevel(Params, MaxDepth);
}

// A coarser neighbor covers a whole edge, so one probe just across each edge's middle finds it.
uint TerrainNeighborLevels(FTerrainRecord* Record, FTerrainNode Node, uint Level)
{
    const int  Last   = Record.Params.Resolution - 1;
    const int  Span   = TerrainNodeSpan(Level);
    const int2 Origin = Node.Coord * Span;
    const int2 Mid    = min(Origin + Span / 2, int2(Last - 1, Last - 1));
    const int2 Probes[4] = { int2(Origin.x - 1, Mid.y), int2(Origin.x + Span, Mid.y), int2(Mid.x, Origin.y - 1), int2(Mid.x, Origin.y + Span) };

    uint Packed = 0u;
    [unroll]
    for (uint Edge = 0u; Edge < 4u; ++Edge)
    {
        const int2 Probe = Probes[Edge];
        uint EdgeLevel = Level;
        if (all(Probe >= 0) && all(Probe < Last))
        {
            EdgeLevel = max(Level, TerrainCutLevelAt(Record, Probe, Node.Depth));
        }
        Packed |= EdgeLevel << (Edge * 4u);
    }
    return Packed;
}

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(uint3 DispatchID : SV_DispatchThreadID)
{
    GTerrainIndex = Pass().TerrainIndex;
    FTerrainRecord* Record = TerrainArgs();

    const uint Depth = Pass().Depth;
    const uint Count = min(Pass().LevelArgs.Get(Depth * kLevelArgsWords + kLevelCountWord), Pass().NodesIn.Count);
    if (DispatchID.x >= Count)
    {
        return;
    }

    const uint         Packed = Pass().NodesIn.Get(DispatchID.x);
    const FTerrainNode Node   = UnpackTerrainNode(Packed);
    float3 BoundsMin, BoundsMax;
    if (!TerrainNodeBounds(Record, Node, BoundsMin, BoundsMax) || !FrustumIntersectsAABB(Scene().CullData.Frustum, BoundsMin, BoundsMax))
    {
        return;
    }

    const FTerrainRenderParams Params = Record.Params;
    const uint Level = TerrainNodeLevel(Params, Depth);

    // Nothing to draw from until the root page lands, which the streamer sends first.
    if (Depth == 0u && !TerrainFindPage(Record, int2(0, 0), (uint)max(Params.PageDepth, 0)).bValid)
    {
        return;
    }

    if (TerrainNodeSplits(Record, Node, Level, BoundsMin, BoundsMax))
    {
        const uint NextArgs = (Depth + 1u) * kLevelArgsWords;
        const uint Slot = AppendReserve(Pass().LevelArgs.At(NextArgs + kLevelCountWord), Pass().NodesOut.Count, 0u, 4u);
        if (Slot != kAppendFull)
        {
            // Reservations are four wide from zero, so exactly the ones opening a group's worth of slots add that group.
            if (Slot % LOCAL_SIZE_X == 0u)
            {
                InterlockedAdd(Pass().LevelArgs.At(NextArgs)[0], 1u);
            }
            [unroll]
            for (uint Child = 0u; Child < 4u; ++Child)
            {
                Pass().NodesOut.Set(Slot + Child, PackTerrainNode(Depth + 1u, Node.Coord * 2 + int2(Child & 1u, Child >> 1u)));
            }
            return;
        }
        // A full child list draws the node coarser rather than dropping it.
    }

    // Base 0 because the CPU rewrites FirstInstance to 0, and reading it would contend with the lanes incrementing InstanceCount.
    const uint VisSlot = AppendReserve(Pass().TerrainIndirect.At(kTerrainDrawInstanceWord), Record.Visible.Count, 0u);
    if (VisSlot == kAppendFull)
    {
        return;
    }
    InterlockedAdd(Pass().TerrainIndirect.At(kTerrainDrawMeshGroupWord)[0], 1u);

    FTerrainVisibleNode Out;
    Out.Node           = Packed;
    Out.NeighborLevels = TerrainNeighborLevels(Record, Node, Level);
    Record.Visible.Set(VisSlot, Out);
}
