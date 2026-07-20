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
    uint NumDraws;
    uint TotalMeshletBound;   // per-view draw-list slice stride
    uint _Pad;
    FDrawIndirectArguments*          IndirectArgs;
    FDrawMeshTasksIndirectArguments* MeshDrawArgs;
    uint*                            DrawMeshletStartOffsets;   // per-draw meshlet prefix (NumDraws entries)
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

    const uint View = Idx / NumDraws;
    const uint Draw = Idx - View * NumDraws;

    // FirstInstance lands inside the view's draw-list slice so the cull's atomic append can't
    // overflow into another view's slice (matches FCullView.DrawListOffset).
    FDrawIndirectArguments Args;
    Args.VertexCount   = MESHLET_VERTICES_PER_DRAW;
    Args.InstanceCount = 0u;
    Args.VertexOffset  = 0u;
    Args.FirstInstance = View * Pass().TotalMeshletBound + Pass().DrawMeshletStartOffsets[Draw];
    Pass().IndirectArgs[Idx] = Args;

    FDrawMeshTasksIndirectArguments MeshArgs;
    MeshArgs.GroupCountX = 0u;
    MeshArgs.GroupCountY = 1u;
    MeshArgs.GroupCountZ = 1u;
    Pass().MeshDrawArgs[Idx] = MeshArgs;
}
