#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHI.slang"

// Resolves each pixel's deferred material once, stores it, and bins each (tile, slot) pair as dense or sparse.

struct FMaterialClassifyArgs
{
    GPUSpan<uint>  Counts;           // zeroed before this pass; dense tiles per slot, then sparse pixels per slot
    GPUSpan<uint>  SlotByMaterial;   // global MaterialIndex -> dense slot (~0u = no deferred draw)
    GPUSpan<uint>  OverflowCount;    // zeroed with Counts, then the pairs appended to Overflow
    GPUSpan<uint>  TileSlots;        // MATERIAL_TILE_RECORD entries per tile, a slot word and two pixel mask words per inline pair
    GPUSpan<uint>  Overflow;         // pairs past a tile's inline entries, tile index above the slot bits
    FTexture2DUInt VisBuffer;
    FRWTexture2D   SlotImage;             // slot + 1 per pixel, 0 where no deferred material shades it
    uint           ScreenW;
    uint           ScreenH;
    uint           DrawListCount;
    uint           _Pad0;
    uint           _Pad1;
    uint           _Pad2;
};
FMaterialClassifyArgs* Pass() { return GetPassArgs<FMaterialClassifyArgs>(); }

groupshared uint gsTileSlotMask[MATERIAL_MAX_SLOTS / 32u];
groupshared uint gsTilePairCount;
groupshared uint gsTileSlots[MATERIAL_TILE_SLOTS];
groupshared uint gsTileSlotPixels[MATERIAL_TILE_SLOTS * 2u];

// The first wave of the tile to claim its slot records the pair, so each (tile, slot) pair is counted once.
void ClaimTileSlot(uint Slot, uint TileIndex)
{
    const uint Bit = 1u << (Slot & 31u);
    uint Previous;
    InterlockedOr(gsTileSlotMask[Slot >> 5u], Bit, Previous);
    if ((Previous & Bit) != 0u)
    {
        return;
    }

    uint Local;
    InterlockedAdd(gsTilePairCount, 1u, Local);
    if (Local < MATERIAL_TILE_SLOTS)
    {
        gsTileSlots[Local] = Slot;
        return;
    }

    // Past the inline entries a pair carries no pixel mask, so it always shades as a whole tile.
    InterlockedAdd(Pass().Counts.At(Slot)[0], 1u);

    uint OverflowIndex;
    InterlockedAdd(Pass().OverflowCount.At(0u)[0], 1u, OverflowIndex);
    if (OverflowIndex < Pass().Overflow.Count)
    {
        Pass().Overflow.Set(OverflowIndex, (TileIndex << MATERIAL_PAIR_SLOT_BITS) | Slot);
    }
}

[shader("compute")]
[numthreads(MATERIAL_CLASSIFY_TILE, MATERIAL_CLASSIFY_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID, uint3 GroupID : SV_GroupID, uint ThreadIndex : SV_GroupIndex)
{
    const uint NumSlots  = min(Pass().Counts.Count / 2u, MATERIAL_MAX_SLOTS);
    const uint TilesX    = (Pass().ScreenW + MATERIAL_CLASSIFY_TILE - 1u) / MATERIAL_CLASSIFY_TILE;
    const uint TileIndex = GroupID.y * TilesX + GroupID.x;
    for (uint Word = ThreadIndex; Word < (NumSlots + 31u) / 32u; Word += MATERIAL_CLASSIFY_TILE * MATERIAL_CLASSIFY_TILE)
    {
        gsTileSlotMask[Word] = 0u;
    }
    if (ThreadIndex < MATERIAL_TILE_SLOTS * 2u)
    {
        gsTileSlotPixels[ThreadIndex] = 0u;
    }
    if (ThreadIndex == 0u)
    {
        gsTilePairCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint Slot    = ~0u;
    bool bShaded = false;
    if (DTid.x < Pass().ScreenW && DTid.y < Pass().ScreenH)
    {
        const uint Raw = Pass().VisBuffer.Load(int2(DTid.xy));
        Slot    = ResolveVisMaterialSlot(Raw, Pass().DrawListCount, Pass().SlotByMaterial);
        bShaded = Slot < NumSlots;

        Pass().SlotImage[int2(DTid.xy)] = float4(bShaded ? float(Slot + 1u) : 0.0, 0.0, 0.0, 0.0);

        // One lane per slot claims it, and lanes exit together since a per-lane exit lets AMD's compiler claim once per wave.
        bool bPending = bShaded;
        [loop]
        while (WaveActiveAnyTrue(bPending))
        {
            if (bPending)
            {
                const uint Leader = WaveReadLaneFirst(Slot);
                if (Slot == Leader)
                {
                    if (WaveIsFirstLane())
                    {
                        ClaimTileSlot(Slot, TileIndex);
                    }
                    bPending = false;
                }
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();
    const uint InlinePairs = min(gsTilePairCount, MATERIAL_TILE_SLOTS);
    if (bShaded)
    {
        for (uint i = 0u; i < InlinePairs; ++i)
        {
            if (gsTileSlots[i] == Slot)
            {
                InterlockedOr(gsTileSlotPixels[i * 2u + (ThreadIndex >> 5u)], 1u << (ThreadIndex & 31u));
                break;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if (ThreadIndex == 0u)
    {
        const uint First = TileIndex * MATERIAL_TILE_RECORD;
        const bool bRecordFits = First + MATERIAL_TILE_RECORD <= Pass().TileSlots.Count;
        [unroll]
        for (uint i = 0u; i < MATERIAL_TILE_SLOTS; ++i)
        {
            uint Word = 0u;
            const uint Lo = gsTileSlotPixels[i * 2u];
            const uint Hi = gsTileSlotPixels[i * 2u + 1u];
            if (i < InlinePairs)
            {
                const uint PairSlot = gsTileSlots[i];
                const uint Pixels   = countbits(Lo) + countbits(Hi);
                const bool bSparse  = Pixels < MATERIAL_SPARSE_PIXELS;
                if (bSparse)
                {
                    InterlockedAdd(Pass().Counts.At(NumSlots + PairSlot)[0], Pixels);
                }
                else
                {
                    InterlockedAdd(Pass().Counts.At(PairSlot)[0], 1u);
                }
                Word = (PairSlot + 1u) | (bSparse ? MATERIAL_TILE_SPARSE_FLAG : 0u);
            }
            if (bRecordFits)
            {
                Pass().TileSlots.Set(First + i * 3u + 0u, Word);
                Pass().TileSlots.Set(First + i * 3u + 1u, Lo);
                Pass().TileSlots.Set(First + i * 3u + 2u, Hi);
            }
        }
    }
}
