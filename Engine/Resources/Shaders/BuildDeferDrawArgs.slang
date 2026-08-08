#include "Includes/SceneGlobals.slang"

// Sizes the LATE views' draw grids from what phase 0 actually deferred.
//
// A late view has no blocks of its own -- its work is exactly the set its EARLY counterpart could not
// resolve against a stale pyramid. So its task grid comes from the early view's defer cursor, and only
// once that pyramid has been rebuilt from this frame's own raster, which is why this runs mid-frame
// rather than alongside BuildDrawPrefix.
//
// One thread per (late view, draw). Fully GPU-side: the args feed CmdDrawMeshTasksIndirect, so no readback
// sits between the count and the draw that consumes it.

struct FDrawMeshTasksIndirectArguments
{
    uint GroupCountX;
    uint GroupCountY;
    uint GroupCountZ;
};

struct FBuildDeferDrawArgs
{
    uint  NumDraws;
    uint  LateViewBase;    // first of the contiguous late views this dispatch covers
    uint  EarlyViewBase;   // its early counterpart, laid out in the same order
    uint  NumLateViews;    // 1 for the camera, NUM_CASCADES for the cascades

    uint* DeferCursors;    // in:  per-(view, draw) entries phase 0 appended
    uint* ViewDrawCounts;  // in:  per-(view, draw) region length, the exact bound on the above
    FDrawMeshTasksIndirectArguments* OutTaskDrawArgs;
};
FBuildDeferDrawArgs* Pass() { return GetPassArgs<FBuildDeferDrawArgs>(); }

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 ThreadID : SV_DispatchThreadID)
{
    const uint NumDraws = Pass().NumDraws;
    const uint Total    = Pass().NumLateViews * NumDraws;
    if (ThreadID.x >= Total)
    {
        return;
    }

    const uint Local = ThreadID.x;
    const uint v     = Local / max(NumDraws, 1u);
    const uint d     = Local % max(NumDraws, 1u);

    const uint SrcIdx = (Pass().EarlyViewBase + v) * NumDraws + d;
    const uint DstIdx = (Pass().LateViewBase  + v) * NumDraws + d;

    // The cursor is bounded by the region it appended into, so this clamp only matters on a frame where
    // the draw allocation itself came up short.
    const uint Deferred = min(Pass().DeferCursors[SrcIdx], Pass().ViewDrawCounts[SrcIdx]);

    FDrawMeshTasksIndirectArguments Args;
    Args.GroupCountX = (Deferred + (MESHLET_TASK_GROUP_SIZE - 1u)) / MESHLET_TASK_GROUP_SIZE;
    Args.GroupCountY = 1u;
    Args.GroupCountZ = 1u;
    Pass().OutTaskDrawArgs[DstIdx] = Args;
}
