#include "Includes/SceneGlobals.slang"

// Clamps the meshlet draw args to the number of draw-list entries that were actually WRITTEN.
//
// CullMeshlets reserves a slot with an atomic bump of InstanceCount and only then discovers whether the
// slot fits in the view's slice. On a frame where the feedback-sized draw list came up short, the bump
// still happened but the write did not -- so the indirect draw would rasterize entries that were never
// filled in, reading whatever the buffer held from an earlier frame. A stale entry's InstanceID is
// unbounded, so the mesh/vertex shader faults on GetInstance() and then on MeshletHeader.
//
// Slots are handed out contiguously (FirstInstance + OldCount), so the written entries are exactly
// [FirstInstance, FirstInstance + min(reserved, MaxCount)). Clamping the published count to MaxCount is
// therefore not an approximation -- it is the precise number of valid entries. Meshlets past it are
// dropped for this frame and the feedback grows the allocation for the next one.
//
// Runs after EVERY cull phase and before that phase's raster: the early raster is issued between the two
// cull phases, so a single clamp at the end would be too late for it.

struct FDrawMeshTasksIndirectArguments
{
    uint GroupCountX;
    uint GroupCountY;
    uint GroupCountZ;
};

struct FClampDrawArgs
{
    uint NumViews;
    uint NumDraws;
    uint _Pad0;
    uint DrawListCapacity;   // entries in the whole allocation, the outer bound

    FDrawIndirectArguments*          IndirectArgs;
    FDrawMeshTasksIndirectArguments* MeshDrawArgs;
    uint*                            ViewDrawCounts;    // per-(view, draw) region length
    uint*                            ViewDrawOffsets;   // per-(view, draw) region base
};
FClampDrawArgs* Pass() { return GetPassArgs<FClampDrawArgs>(); }

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 ThreadID : SV_DispatchThreadID)
{
    const uint NumDraws = Pass().NumDraws;
    const uint Total    = Pass().NumViews * NumDraws;
    const uint Idx      = ThreadID.x;
    if (Idx >= Total)
    {
        return;
    }

    // This region's exact length, and never past the allocation. Same bound EmitMeshlet clamps against,
    // and read from the same source, so this pass does not depend on SeedIndirectArgs having run.
    const uint FirstInstance = Pass().ViewDrawOffsets[Idx];
    const uint RegionEnd     = min(FirstInstance + Pass().ViewDrawCounts[Idx], Pass().DrawListCapacity);
    const uint MaxCount      = (RegionEnd > FirstInstance) ? (RegionEnd - FirstInstance) : 0u;

    Pass().IndirectArgs[Idx].InstanceCount = min(Pass().IndirectArgs[Idx].InstanceCount, MaxCount);
    Pass().MeshDrawArgs[Idx].GroupCountX   = min(Pass().MeshDrawArgs[Idx].GroupCountX,   MaxCount);
}
