#include "Includes/SceneGlobals.slang"

// Seeds the per-view indirect draw args + mesh-task args the meshlet cull accumulates into. One thread
// per (view, draw) slot expands the D-entry per-draw meshlet prefix into the V*D seed arrays, replacing
// the CPU fill + upload (28 bytes per slot per frame) with a D-uint transient upload and this dispatch.
// Runs at the top of the early cull, before anything reads or atomically bumps the args.

// Mirror of CullMeshlets.slang (Y/Z stay 1; the cull accumulates GroupCountX).
struct FDrawMeshTasksIndirectArguments
{
    uint GroupCountX;
    uint GroupCountY;
    uint GroupCountZ;
};

struct FSeedIndirectArgs
{
    uint NumViews;
    uint NumDraws;            // = batch count; CPU-known, since a draw is a PSO bucket
    uint _Pad0;
    uint _Pad1;
    FDrawIndirectArguments*          IndirectArgs;
    FDrawMeshTasksIndirectArguments* MeshDrawArgs;
    // Per-(view, draw) start offset into the meshlet draw list, GPU-built by BuildDrawPrefix from the
    // exact emit bounds. There is no per-view stride any more -- regions are packed, so a view that culls
    // heavily takes almost no space instead of reserving an equal share of the worst case.
    uint*                            ViewDrawOffsets;
};
FSeedIndirectArgs* Pass() { return GetPassArgs<FSeedIndirectArgs>(); }

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

    // FirstInstance is this (view, draw)'s base in the meshlet draw list. Both raster paths read it: the VS
    // path through gl_InstanceIndex (= FirstInstance + instance), the mesh path by loading
    // IndirectArgs[ArgBase].FirstInstance directly. So writing the packed offset here is all it takes to
    // move the whole renderer off equal per-view slices.
    FDrawIndirectArguments Args;
    Args.VertexCount   = MESHLET_VERTICES_PER_DRAW;
    Args.InstanceCount = 0u;
    Args.VertexOffset  = 0u;
    Args.FirstInstance = Pass().ViewDrawOffsets[Idx];
    Pass().IndirectArgs[Idx] = Args;

    FDrawMeshTasksIndirectArguments MeshArgs;
    MeshArgs.GroupCountX = 0u;
    MeshArgs.GroupCountY = 1u;
    MeshArgs.GroupCountZ = 1u;
    Pass().MeshDrawArgs[Idx] = MeshArgs;
}
