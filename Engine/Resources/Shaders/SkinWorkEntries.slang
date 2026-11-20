#include "Includes/SceneGlobals.slang"

// One thread per skinned entry stamps its index over its run of work, so a skinning group finds its entry in one load.

struct FSkinWorkEntriesArgs
{
    GPUSpan<uint>              SlotList;
    GPUSpan<FSkinnedFrameData> SkinnedData;
    GPUSpan<uint>              WorkBase;      // BuildSkinWork.slang's running base per entry
    GPUSpan<uint>              OutWorkEntry;
};
FSkinWorkEntriesArgs* Pass() { return GetPassArgs<FSkinWorkEntriesArgs>(); }

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    const uint Entry = (GroupID.x + GroupID.y * (uint)MAX_DISPATCH_AXIS) * 64u + Lane;
    if (Entry >= Pass().SlotList.Count || !Pass().WorkBase.Contains(Entry))
    {
        return;
    }

    const FSkinnedFrameData Data = Pass().SkinnedData.Get(Pass().SlotList.Get(Entry));
    if (!HasPreSkinSlice(Data.SkinnedVertexBase))
    {
        return;
    }

    const uint Begin = Pass().WorkBase.Get(Entry);
    const uint End   = min(Begin + Data.SurfaceMeshletCount, Pass().OutWorkEntry.Count);
    for (uint Work = Begin; Work < End; ++Work)
    {
        Pass().OutWorkEntry.Set(Work, Entry);
    }
}
