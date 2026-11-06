#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/WaveAtomics.slang"
#include "Includes/SelectionOutlineCommon.slang"

// Lists every 8x8 tile within outline reach of a selected pixel, once each, as the instances SelectionOutlineTileQuad.slang draws.
struct FSelectionTileArgs
{
    GPUSpan<uint>  SelectionBits;
    GPUSpan<uint>  TileStamps;
    GPUSpan<uint>  TileList;
    GPUSpan<uint>  DrawArgs;
    FTexture2DUInt Picker;
    uint           EntityIndexMask;
    uint           TileStamp;
    uint           TilesX;
    uint           TilesY;
    uint           TileReach;
    uint           _Pad0;
    uint           _Pad1;
};
FSelectionTileArgs* Pass() { return GetPassArgs<FSelectionTileArgs>(); }

groupshared uint GTileHasSelection;

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
    if (LocalIndex == 0u)
    {
        GTileHasSelection = 0u;
        if (all(GroupID.xy == uint2(0u, 0u)))
        {
            // Six vertices make one tile quad, and the instance count is what the appends below add up.
            Pass().DrawArgs.Set(0u, 6u);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 Extent = int2(GetDisplaySize());
    const int2 Texel  = int2(GroupID.xy * 8u + LocalID.xy);
    if (all(Texel < Extent) && IsEntitySelected(Pass().Picker.Load(DisplayToPickerTexel(Texel)), Pass().SelectionBits, Pass().EntityIndexMask))
    {
        InterlockedOr(GTileHasSelection, 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    // A selected pixel reaches its own tile and every tile within the outline radius of it.
    const uint Side = Pass().TileReach * 2u + 1u;
    if (GTileHasSelection == 0u || LocalIndex >= Side * Side)
    {
        return;
    }

    const int2 Tile = int2(GroupID.xy) + int2(int(LocalIndex % Side), int(LocalIndex / Side)) - int(Pass().TileReach);
    if (any(Tile < int2(0, 0)) || Tile.x >= int(Pass().TilesX) || Tile.y >= int(Pass().TilesY))
    {
        return;
    }

    // The stamp swap lets exactly one of the selected neighbors list a shared tile.
    uint* Stamp = Pass().TileStamps.At(uint(Tile.y) * Pass().TilesX + uint(Tile.x));
    uint Previous = 0u;
    InterlockedExchange(Stamp[0], Pass().TileStamp, Previous);
    if (Previous == Pass().TileStamp)
    {
        return;
    }

    const uint Slot = WaveInterlockedAdd(Pass().DrawArgs.At(1u), 1u);
    Pass().TileList.Set(Slot, uint(Tile.x) | (uint(Tile.y) << 16u));
}
