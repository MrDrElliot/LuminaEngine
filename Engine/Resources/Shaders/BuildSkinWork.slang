#include "Includes/SceneGlobals.slang"

// One workgroup per (skinned slot, meshlet), keyed on retained slots that all hold this frame's data.

struct FDispatchIndirectArguments
{
    uint GroupX;
    uint GroupY;
    uint GroupZ;
};

struct FBuildSkinWorkArgs
{
    GPUSpan<uint>              SlotList;
    GPUSpan<FSkinnedFrameData> SkinnedData;   // indexed by retained slot, not by SlotList position

    // One running base per slot, written for empty slots too, so Skinning.slang's search never reads a stale one.
    GPUSpan<uint> OutWorkBase;
    GPUSpan<FDispatchIndirectArguments> OutDispatchArgs;
};
FBuildSkinWorkArgs* Pass() { return GetPassArgs<FBuildSkinWorkArgs>(); }

// A slot granted no slice skins inline in the draw path, so it pre-skins nothing.
uint SkinMeshletCount(FSkinnedFrameData Data)
{
    if (Data.SkinnedVertexBase == kNoPreSkinBase)
    {
        return 0u;
    }
    return Data.SurfaceMeshletCount;
}

#define SKIN_WORK_GROUP 256

groupshared uint gsScan[SKIN_WORK_GROUP];
groupshared uint gsRunning;

// No lane may return early, or the barriers below go non-uniform and AMD hangs.
[shader("compute")]
[numthreads(SKIN_WORK_GROUP, 1, 1)]
void ComputeMain(uint Lane : SV_GroupIndex)
{
    const uint NumSlots = Pass().SlotList.Count;

    if (Lane == 0u)
    {
        gsRunning = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // The running base is sequential across tiles, but the 256 loads inside one are not.
    for (uint TileBase = 0u; TileBase < NumSlots; TileBase += SKIN_WORK_GROUP)
    {
        const uint Entry = TileBase + Lane;

        uint Count = 0u;
        if (Entry < NumSlots)
        {
            Count = SkinMeshletCount(Pass().SkinnedData.Get(Pass().SlotList.Get(Entry)));
        }

        gsScan[Lane] = Count;
        GroupMemoryBarrierWithGroupSync();

        for (uint Offset = 1u; Offset < SKIN_WORK_GROUP; Offset <<= 1u)
        {
            const uint Add = (Lane >= Offset) ? gsScan[Lane - Offset] : 0u;
            GroupMemoryBarrierWithGroupSync();
            gsScan[Lane] += Add;
            GroupMemoryBarrierWithGroupSync();
        }

        const uint Inclusive = gsScan[Lane];

        if (Entry < NumSlots)
        {
            Pass().OutWorkBase.Set(Entry, gsRunning + (Inclusive - Count));
        }

        const uint TileTotal = gsScan[SKIN_WORK_GROUP - 1u];
        GroupMemoryBarrierWithGroupSync();
        if (Lane == 0u)
        {
            gsRunning += TileTotal;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (Lane == 0u)
    {
        // Folded across Y exactly as every other oversized grid in the renderer; Skinning.slang undoes it.
        FDispatchIndirectArguments Args;
        Args.GroupX = min(gsRunning, (uint)MAX_DISPATCH_AXIS);
        Args.GroupY = (gsRunning + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
        Args.GroupZ = 1u;
        Pass().OutDispatchArgs.Set(0u, Args);
    }
}
