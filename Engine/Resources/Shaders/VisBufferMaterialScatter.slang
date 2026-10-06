#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"

// Moves each (tile, slot) pair the classify recorded into its slot's run, a thread per tile rather than per pixel.

struct FMaterialScatterArgs
{
    GPUSpan<uint> Offsets;    // dense run starts per slot, then sparse run starts
    GPUSpan<uint> Cursors;    // zeroed by the args pass, same two halves
    GPUSpan<uint> PairList;   // packed tile coordinates, grouped by slot
    GPUSpan<uint> TileSlots;  // MATERIAL_TILE_RECORD entries per tile
    GPUSpan<uint> Overflow;   // tile index above the slot bits
    GPUSpan<uint> OverflowCount;  // the pairs the classify appended to Overflow
    GPUSpan<uint> PixelList;  // packed pixel coordinates of sparse pairs, grouped by slot
    uint  TilesX;
    uint  NumTiles;
    uint  NumSlots;
    uint  _Pad0;
};
FMaterialScatterArgs* Pass() { return GetPassArgs<FMaterialScatterArgs>(); }

uint2 TileOrigin(uint Tile)
{
    return uint2(Tile % Pass().TilesX, Tile / Pass().TilesX);
}

// Lanes sharing a slot take one atomic between them, since one hot slot would otherwise serialize every lane.
void AppendPair(uint Slot, uint Tile)
{
    [loop]
    while (true)
    {
        const uint Leader = WaveReadLaneFirst(Slot);
        if (Slot != Leader)
        {
            continue;
        }

        const uint Rank  = WavePrefixCountBits(true);
        const uint Count = WaveActiveCountBits(true);
        uint Base = 0u;
        if (Rank == 0u)
        {
            InterlockedAdd(SpanAt(&Pass().Cursors, Slot)[0], Count, Base);
        }
        Base = WaveReadLaneFirst(Base);

        const uint Entry = SpanGet(&Pass().Offsets, Slot) + Base + Rank;
        if (Entry < Pass().PairList.Count)
        {
            SpanSet(&Pass().PairList, Entry, PackVisPixel(TileOrigin(Tile)));
        }
        break;
    }
}

// Lists each covered pixel of a thin pair on its own, so the material pass spends a lane per pixel instead of a tile.
void AppendPixels(uint Slot, uint Tile, uint Lo, uint Hi)
{
    const uint Run = Pass().NumSlots + Slot;
    [loop]
    while (true)
    {
        const uint Leader = WaveReadLaneFirst(Slot);
        if (Slot != Leader)
        {
            continue;
        }

        const uint Pixels = countbits(Lo) + countbits(Hi);
        const uint Before = WavePrefixSum(Pixels);
        const uint Total  = WaveActiveSum(Pixels);
        uint Base = 0u;
        if (WaveIsFirstLane())
        {
            InterlockedAdd(SpanAt(&Pass().Cursors, Run)[0], Total, Base);
        }
        Base = WaveReadLaneFirst(Base);

        const uint2 Origin = TileOrigin(Tile) * MATERIAL_CLASSIFY_TILE;
        uint Entry = SpanGet(&Pass().Offsets, Run) + Base + Before;
        for (uint Half = 0u; Half < 2u; ++Half)
        {
            uint Bits = Half == 0u ? Lo : Hi;
            while (Bits != 0u)
            {
                const uint Bit   = firstbitlow(Bits);
                const uint Local = Half * 32u + Bit;
                Bits &= Bits - 1u;
                if (Entry < Pass().PixelList.Count)
                {
                    SpanSet(&Pass().PixelList, Entry, PackVisPixel(Origin + uint2(Local % MATERIAL_CLASSIFY_TILE, Local / MATERIAL_CLASSIFY_TILE)));
                }
                ++Entry;
            }
        }
        break;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint ThreadIndex : SV_GroupIndex)
{
    const uint Index    = (GroupID.y * (uint)MAX_DISPATCH_AXIS + GroupID.x) * 64u + ThreadIndex;
    const uint NumSlots = min(Pass().NumSlots, MATERIAL_MAX_SLOTS);

    if (Index < Pass().NumTiles)
    {
        const uint First = Index * MATERIAL_TILE_RECORD;
        [unroll]
        for (uint i = 0u; i < MATERIAL_TILE_SLOTS; ++i)
        {
            const uint Word   = SpanGet(&Pass().TileSlots, First + i * 3u);
            const uint Stored = Word & ~MATERIAL_TILE_SPARSE_FLAG;
            if (Stored == 0u || Stored > NumSlots)
            {
                continue;
            }

            if ((Word & MATERIAL_TILE_SPARSE_FLAG) != 0u)
            {
                AppendPixels(Stored - 1u, Index, SpanGet(&Pass().TileSlots, First + i * 3u + 1u), SpanGet(&Pass().TileSlots, First + i * 3u + 2u));
            }
            else
            {
                AppendPair(Stored - 1u, Index);
            }
        }
        return;
    }

    const uint OverflowIndex = Index - Pass().NumTiles;
    if (OverflowIndex < min(SpanGet(&Pass().OverflowCount, 0u), Pass().Overflow.Count))
    {
        const uint Packed = SpanGet(&Pass().Overflow, OverflowIndex);
        const uint Slot   = Packed & MATERIAL_PAIR_SLOT_MASK;
        if (Slot < NumSlots)
        {
            AppendPair(Slot, Packed >> MATERIAL_PAIR_SLOT_BITS);
        }
    }
}
