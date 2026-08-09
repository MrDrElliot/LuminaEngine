#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"

// Deferred material classification, step 3: write every classified pixel's screen position into its
// material's run of the pixel list.
//
// Each tile counts its own pixels first, reserves ONE contiguous range per material it contains, then
// writes into that range with a groupshared index. Two consequences, both wanted: the global cursors take
// one atomic per (tile, material) instead of one per pixel, and a material's run comes out grouped by
// screen tile, so the per-material pass that walks it later hits the same triangles and textures across
// neighboring threads instead of jumping across the frame.
//
// The slot resolve is the same call VisBufferMaterialCount makes, over the same VisBuffer, so the run
// sized there is exactly the run filled here.

struct FMaterialScatterArgs
{
    uint* Cursors;              // uint[MATERIAL_MAX_SLOTS], seeded to Starts by the prefix sum
    uint* PixelList;            // uint[PixelListCapacity], packed 16:16 screen position
    uint* SlotByMaterial;
    uint  VisBufferIndex;
    uint  ScreenW;
    uint  ScreenH;
    uint  DrawListCount;
    uint  SlotByMaterialCount;
    uint  NumSlots;
    uint  PixelListCapacity;
    uint  _Pad0;
};
FMaterialScatterArgs* Pass() { return GetPassArgs<FMaterialScatterArgs>(); }

#define CLASSIFY_GROUP_THREADS (MATERIAL_CLASSIFY_TILE * MATERIAL_CLASSIFY_TILE)

groupshared uint GroupCounts[MATERIAL_MAX_SLOTS];
groupshared uint GroupBase[MATERIAL_MAX_SLOTS];

[shader("compute")]
[numthreads(MATERIAL_CLASSIFY_TILE, MATERIAL_CLASSIFY_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID, uint Lane : SV_GroupIndex)
{
    for (uint i = Lane; i < MATERIAL_MAX_SLOTS; i += CLASSIFY_GROUP_THREADS)
    {
        GroupCounts[i] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint Slot       = ~0u;
    uint LocalIndex = 0u;
    if (DTid.x < Pass().ScreenW && DTid.y < Pass().ScreenH)
    {
        const uint Raw      = LoadTexture2DUInt(Pass().VisBufferIndex, int2(DTid.xy));
        const uint Resolved = ResolveVisMaterialSlot(Raw, Pass().DrawListCount,
                                                     Pass().SlotByMaterial, Pass().SlotByMaterialCount);
        if (Resolved < Pass().NumSlots)
        {
            Slot = Resolved;
            InterlockedAdd(GroupCounts[Slot], 1u, LocalIndex);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint i = Lane; i < MATERIAL_MAX_SLOTS; i += CLASSIFY_GROUP_THREADS)
    {
        uint Base = 0u;
        if (GroupCounts[i] != 0u)
        {
            InterlockedAdd(Pass().Cursors[i], GroupCounts[i], Base);
        }
        GroupBase[i] = Base;
    }
    GroupMemoryBarrierWithGroupSync();

    if (Slot != ~0u)
    {
        // The list holds one entry per screen pixel and the counts came from that same screen, so this
        // cannot overflow. Bounded anyway: the cost is one compare, and the alternative failure is a
        // stray write past a device allocation.
        const uint Index = GroupBase[Slot] + LocalIndex;
        if (Index < Pass().PixelListCapacity)
        {
            Pass().PixelList[Index] = PackVisPixel(DTid.xy);
        }
    }
}
