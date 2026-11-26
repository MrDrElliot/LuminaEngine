#include "Includes/SceneGlobals.slang"
#include "Includes/WaveAtomics.slang"

// One workgroup per (skinned entry, meshlet), keyed on skinned indices that all hold this frame's data.

struct FBuildSkinWorkArgs
{
    GPUSpan<uint>              SlotList;
    GPUSpan<FSkinnedFrameData> SkinnedData;   // indexed by the skinned index SlotList holds, not by list position

    // One base per slot, written for empty slots too, since Skinning.slang subtracts it from the work index.
    GPUSpan<uint> OutWorkBase;
    // The X word of the skinning dispatch, cleared before this pass and folded into a grid by SkinWorkEntries.slang.
    GPUSpan<uint> OutGroupCount;
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

// Runs need not follow list order, since a skinning group reads its entry's base back instead of searching for it.
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    // Undoes the Y fold of the dispatch past MAX_DISPATCH_AXIS groups.
    const uint Entry  = (GroupID.x + GroupID.y * (uint)MAX_DISPATCH_AXIS) * 64u + Lane;
    const bool bEntry = Entry < Pass().SlotList.Count;
    const uint Count  = bEntry ? SkinMeshletCount(Pass().SkinnedData.Get(Pass().SlotList.Get(Entry))) : 0u;

    // Every lane reaches the add, so one atomic serves the whole wave.
    const uint Base = WaveInterlockedAdd(Pass().OutGroupCount.At(0u), Count);
    if (bEntry && Pass().OutWorkBase.Contains(Entry))
    {
        Pass().OutWorkBase.Set(Entry, Base);
    }
}
