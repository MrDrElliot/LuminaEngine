#include "Includes/SceneGlobals.slang"

// One thread per skinned entry stamps its index over its run of work, so a skinning group finds its entry in one load.

struct FDispatchIndirectArguments
{
    uint GroupX;
    uint GroupY;
    uint GroupZ;
};

struct FSkinWorkEntriesArgs
{
    GPUSpan<uint>              SlotList;
    GPUSpan<FSkinnedFrameData> SkinnedData;
    GPUSpan<uint>              WorkBase;      // BuildSkinWork.slang's base per entry
    GPUSpan<uint>              OutWorkEntry;
    // Holds BuildSkinWork.slang's total in GroupX until the first thread folds it into the grid Skinning.slang runs on.
    GPUSpan<FDispatchIndirectArguments> OutDispatchArgs;
};
FSkinWorkEntriesArgs* Pass() { return GetPassArgs<FSkinWorkEntriesArgs>(); }

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    const uint Entry = (GroupID.x + GroupID.y * (uint)MAX_DISPATCH_AXIS) * 64u + Lane;

    if (Entry == 0u && Pass().OutDispatchArgs.Contains(0u))
    {
        // Folded across Y exactly as every other oversized grid in the renderer; Skinning.slang undoes it.
        const uint Groups = min(Pass().OutDispatchArgs.Get(0u).GroupX, Pass().OutWorkEntry.Count);
        FDispatchIndirectArguments Args;
        Args.GroupX = min(Groups, (uint)MAX_DISPATCH_AXIS);
        Args.GroupY = (Groups + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
        Args.GroupZ = 1u;
        Pass().OutDispatchArgs.Set(0u, Args);
    }

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
