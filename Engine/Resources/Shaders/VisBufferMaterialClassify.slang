#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHIStorage.slang"

// Resolves each pixel's deferred material once, stores it, and bins every 8x8 tile under each material it contains.

struct FMaterialClassifyArgs
{
    GPUSpan<uint> Counts;           // zeroed before this pass, then the tiles each slot owns
    GPUSpan<uint> SlotByMaterial;   // global MaterialIndex -> dense slot (~0u = no deferred draw)
    GPUSpan<uint> TileList;         // one run of TileStride packed tile coordinates per slot
    uint  VisBufferIndex;
    uint  SlotImageUAV;             // slot + 1 per pixel, 0 where no deferred material shades it
    uint  ScreenW;
    uint  ScreenH;
    uint  DrawListCount;
    uint  TileStride;
    uint  _Pad0;
    uint  _Pad1;
};
FMaterialClassifyArgs* Pass() { return GetPassArgs<FMaterialClassifyArgs>(); }

groupshared uint gTileSlotMask[MATERIAL_MAX_SLOTS / 32u];

[shader("compute")]
[numthreads(MATERIAL_CLASSIFY_TILE, MATERIAL_CLASSIFY_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID, uint3 GroupID : SV_GroupID, uint ThreadIndex : SV_GroupIndex)
{
    const uint NumSlots = min(Pass().Counts.Count, MATERIAL_MAX_SLOTS);
    for (uint Word = ThreadIndex; Word < (NumSlots + 31u) / 32u; Word += MATERIAL_CLASSIFY_TILE * MATERIAL_CLASSIFY_TILE)
    {
        gTileSlotMask[Word] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    if (DTid.x >= Pass().ScreenW || DTid.y >= Pass().ScreenH)
    {
        return;
    }

    const uint Raw     = LoadTexture2DUInt(Pass().VisBufferIndex, int2(DTid.xy));
    const uint Slot    = ResolveVisMaterialSlot(Raw, Pass().DrawListCount, &Pass().SlotByMaterial);
    const bool bShaded = Slot < NumSlots;

    gRWTextures2D[Pass().SlotImageUAV][int2(DTid.xy)] = float4(bShaded ? float(Slot + 1u) : 0.0, 0.0, 0.0, 0.0);
    if (!bShaded)
    {
        return;
    }

    // The first pixel of the tile to claim its slot appends the tile, so each (tile, slot) pair is listed once.
    const uint Bit = 1u << (Slot & 31u);
    uint Previous;
    InterlockedOr(gTileSlotMask[Slot >> 5u], Bit, Previous);
    if ((Previous & Bit) != 0u)
    {
        return;
    }

    uint Index;
    InterlockedAdd(SpanAt(&Pass().Counts, Slot)[0], 1u, Index);
    if (Index < Pass().TileStride)
    {
        SpanSet(&Pass().TileList, Slot * Pass().TileStride + Index, PackVisPixel(GroupID.xy));
    }
}
