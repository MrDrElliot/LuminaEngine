#include "Includes/SceneGlobals.slang"
#include "Includes/MaterialTiles.slang"

// Compact MaterialDepth's per-tile slot bitmask into one packed tile list per material slot, and fill in the
// per-slot indirect draw args from the counts.
//
// This is what turns the material pass from "every material rasterizes every tile on screen and the vertex
// shader throws away the ones it does not cover" into "every material rasterizes exactly its own tiles".
// The self-culling variant still paid a vertex-shader launch and a bitmask load per (tile, material), which
// at 1080p is ~8100 degenerate quads per material; here the tile count IS the draw's instance count.
//
// One thread per tile, walking that tile's bitmask words. Slots are laid out at a fixed stride of TotalTiles,
// so no prefix sum is needed and the bound is exact: the bitmask holds one bit per (tile, slot), so a slot
// can be appended at most once per tile and therefore at most TotalTiles times.

struct FBuildTileListArgs
{
    FDrawIndirectArguments* Args;       // one per slot, pre-seeded {6, 0, 0, Slot * TotalTiles}
    uint*                   TileList;  // uint[NumSlots * TotalTiles]: tile indices, Slot * TotalTiles strided
    uint*                   TileBits;  // uint[TotalTiles * TileWordCount] from MaterialDepth
    uint                    TotalTiles;
    uint                    NumSlots;
    uint                    TileWordCount;
    uint                    _Pad0;
};
FBuildTileListArgs* Pass() { return GetPassArgs<FBuildTileListArgs>(); }

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint Tile = DTid.x;
    if (Tile >= Pass().TotalTiles)
    {
        return;
    }

    const uint WordCount = Pass().TileWordCount;
    const uint NumSlots  = Pass().NumSlots;

    for (uint Word = 0u; Word < WordCount; ++Word)
    {
        uint Bits = Pass().TileBits[Tile * WordCount + Word];

        // Iterate only the set bits; a tile typically holds one or two materials, so this loop runs far
        // fewer times than NumSlots.
        while (Bits != 0u)
        {
            const uint Bit  = firstbitlow(Bits);
            Bits           &= Bits - 1u;

            const uint Slot = Word * 32u + Bit;
            if (Slot >= NumSlots)
            {
                continue;   // padding bits in the last word
            }

            uint Local;
            InterlockedAdd(Pass().Args[Slot].InstanceCount, 1u, Local);

            // Exact by construction (one append per tile per slot), but the capacity travels in the push
            // block beside the atomic regardless: an unbounded atomic used directly as a write index is the
            // shape that took out the meshlet defer list.
            if (Local >= Pass().TotalTiles)
            {
                continue;
            }

            // Base computed arithmetically rather than loaded from Args[Slot].FirstInstance: that field
            // shares its 16 bytes with the InstanceCount other lanes are atomically incrementing, and
            // reading a struct field out of a contended cacheline is exactly what the meshlet cull stopped
            // doing. The CPU seeds FirstInstance to this same value for the draw itself.
            Pass().TileList[Slot * Pass().TotalTiles + Local] = Tile;
        }
    }
}
