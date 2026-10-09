#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// Lists every 8x8 tile within outline reach of a selected pixel, once each, as the instances SelectionOutlineTileQuad.slang draws.
struct FSelectionTileArgs
{
    GPUSpan<uint> SelectionBits;
    GPUSpan<uint> TileStamps;
    GPUSpan<uint> TileList;
    GPUSpan<uint> DrawArgs;
    uint PickerIndex;
    uint EntityIndexMask;
    uint TileStamp;
    uint TilesX;
    uint TilesY;
    uint TileReach;
    uint _Pad0;
    uint _Pad1;
};
FSelectionTileArgs* Pass() { return GetPassArgs<FSelectionTileArgs>(); }

// The same membership test as SelectionOutline.slang, so a tile is listed exactly when that pass would find a selected pixel in it.
bool IsEntitySelected(uint PickerValue)
{
    if (PickerValue == 0u || PickerValue == 0xFFFFFFFFu)
    {
        return false;
    }

    const uint Index = PickerValue & Pass().EntityIndexMask;
    return (SpanGet(&Pass().SelectionBits, Index >> 5u) & (1u << (Index & 31u))) != 0u;
}

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
            SpanSet(&Pass().DrawArgs, 0u, 6u);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 Extent = int2(Scene().ScreenSize.xy);
    const int2 Texel  = int2(GroupID.xy * 8u + LocalID.xy);
    if (all(Texel < Extent) && IsEntitySelected(LoadTexture2DUInt(Pass().PickerIndex, Texel)))
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
    uint* Stamp = SpanAt(&Pass().TileStamps, uint(Tile.y) * Pass().TilesX + uint(Tile.x));
    uint Previous = 0u;
    InterlockedExchange(Stamp[0], Pass().TileStamp, Previous);
    if (Previous == Pass().TileStamp)
    {
        return;
    }

    uint* InstanceCount = SpanAt(&Pass().DrawArgs, 1u);
    uint Slot = 0u;
    InterlockedAdd(InstanceCount[0], 1u, Slot);
    SpanSet(&Pass().TileList, Slot, uint(Tile.x) | (uint(Tile.y) << 16u));
}
