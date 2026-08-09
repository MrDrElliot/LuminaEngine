#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"

// Deferred material classification, step 1: how many pixels each material owns.
//
// One thread per screen pixel, aggregated through groupshared memory before the global counters are
// touched: a tile contributes ONE atomic per material it contains rather than one per pixel. Straight to
// global would be ~2M atomics onto 64 addresses at 1080p, all serializing on the same cache lines.
//
// The slot resolve here MUST match VisBufferMaterialScatter's exactly -- they walk the same VisBuffer and
// a disagreement would size a material's run differently from what gets written into it. That is why both
// call ResolveVisMaterialSlot rather than decoding the VisID themselves.

struct FMaterialCountArgs
{
    uint* Counts;               // uint[MATERIAL_MAX_SLOTS], zeroed before this pass
    uint* SlotByMaterial;       // global MaterialIndex -> dense slot (~0u = no deferred draw)
    uint  VisBufferIndex;
    uint  ScreenW;
    uint  ScreenH;
    uint  DrawListCount;
    uint  SlotByMaterialCount;
    uint  NumSlots;
};
FMaterialCountArgs* Pass() { return GetPassArgs<FMaterialCountArgs>(); }

#define CLASSIFY_GROUP_THREADS (MATERIAL_CLASSIFY_TILE * MATERIAL_CLASSIFY_TILE)

groupshared uint GroupCounts[MATERIAL_MAX_SLOTS];

[shader("compute")]
[numthreads(MATERIAL_CLASSIFY_TILE, MATERIAL_CLASSIFY_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID, uint Lane : SV_GroupIndex)
{
    for (uint i = Lane; i < MATERIAL_MAX_SLOTS; i += CLASSIFY_GROUP_THREADS)
    {
        GroupCounts[i] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    if (DTid.x < Pass().ScreenW && DTid.y < Pass().ScreenH)
    {
        const uint Raw  = LoadTexture2DUInt(Pass().VisBufferIndex, int2(DTid.xy));
        const uint Slot = ResolveVisMaterialSlot(Raw, Pass().DrawListCount,
                                                 Pass().SlotByMaterial, Pass().SlotByMaterialCount);
        if (Slot < Pass().NumSlots)
        {
            InterlockedAdd(GroupCounts[Slot], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint i = Lane; i < MATERIAL_MAX_SLOTS; i += CLASSIFY_GROUP_THREADS)
    {
        const uint Local = GroupCounts[i];
        if (i < Pass().NumSlots && Local != 0u)
        {
            InterlockedAdd(Pass().Counts[i], Local);
        }
    }
}
