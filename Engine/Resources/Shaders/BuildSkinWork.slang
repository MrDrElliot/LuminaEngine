#include "Includes/SceneGlobals.slang"
#include "Includes/GroupScan.slang"

// One workgroup per (skinned entry, meshlet), keyed on skinned indices that all hold this frame's data.

struct FDispatchIndirectArguments
{
    uint GroupX;
    uint GroupY;
    uint GroupZ;
};

struct FBuildSkinWorkArgs
{
    GPUSpan<uint>              SlotList;
    GPUSpan<FSkinnedFrameData> SkinnedData;   // indexed by the skinned index SlotList holds, not by list position

    // One running base per slot, written for empty slots too, since Skinning.slang subtracts it from the work index.
    GPUSpan<uint> OutWorkBase;
    GPUSpan<FDispatchIndirectArguments> OutDispatchArgs;

    // Only its count is read here, since SkinWorkEntries.slang fills it and the dispatch must not outrun it.
    GPUSpan<uint> WorkEntry;
};
FBuildSkinWorkArgs* Pass() { return GetPassArgs<FBuildSkinWorkArgs>(); }

// A slot granted no slice skins inline in the draw path, so it pre-skins nothing.
uint SkinMeshletCount(FSkinnedFrameData Data)
{
    if (!HasPreSkinSlice(Data.SkinnedVertexBase))
    {
        return 0u;
    }
    return Data.SurfaceMeshletCount;
}

uint EntryMeshletCount(uint Entry)
{
    return SkinMeshletCount(Pass().SkinnedData.Get(Pass().SlotList.Get(Entry)));
}

#define SKIN_WORK_GROUP 1024u

// Loads issued together, since a lone dependent load per iteration would serialize on its latency.
#define SKIN_WORK_BATCH 8u

// Each thread owns a contiguous run of entries, so one group-wide scan orders them all. No lane may return early.
[shader("compute")]
[numthreads(SKIN_WORK_GROUP, 1, 1)]
void ComputeMain(uint Lane : SV_GroupIndex)
{
    const uint NumSlots  = Pass().SlotList.Count;
    const uint PerThread = (NumSlots + SKIN_WORK_GROUP - 1u) / SKIN_WORK_GROUP;
    const uint Begin     = min(Lane * PerThread, NumSlots);
    const uint End       = min(Begin + PerThread, NumSlots);

    uint Sum = 0u;
    for (uint Base = Begin; Base < End; Base += SKIN_WORK_BATCH)
    {
        uint Counts[SKIN_WORK_BATCH];
        [unroll]
        for (uint k = 0u; k < SKIN_WORK_BATCH; ++k)
        {
            Counts[k] = (Base + k < End) ? EntryMeshletCount(Base + k) : 0u;
        }
        [unroll]
        for (uint k = 0u; k < SKIN_WORK_BATCH; ++k)
        {
            Sum += Counts[k];
        }
    }

    uint2 GroupTotal;
    const uint ThreadBase = GroupExclusiveScan(uint2(Sum, 0u), Lane, SKIN_WORK_GROUP, GroupTotal).x;

    uint Running = ThreadBase;
    for (uint Base = Begin; Base < End; Base += SKIN_WORK_BATCH)
    {
        uint Counts[SKIN_WORK_BATCH];
        [unroll]
        for (uint k = 0u; k < SKIN_WORK_BATCH; ++k)
        {
            Counts[k] = (Base + k < End) ? EntryMeshletCount(Base + k) : 0u;
        }
        [unroll]
        for (uint k = 0u; k < SKIN_WORK_BATCH; ++k)
        {
            if (Base + k < End)
            {
                Pass().OutWorkBase.Set(Base + k, Running);
            }
            Running += Counts[k];
        }
    }

    if (Lane == 0u)
    {
        // Folded across Y exactly as every other oversized grid in the renderer; Skinning.slang undoes it.
        const uint Groups = min(GroupTotal.x, Pass().WorkEntry.Count);
        FDispatchIndirectArguments Args;
        Args.GroupX = min(Groups, (uint)MAX_DISPATCH_AXIS);
        Args.GroupY = (Groups + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
        Args.GroupZ = 1u;
        Pass().OutDispatchArgs.Set(0u, Args);
    }
}
