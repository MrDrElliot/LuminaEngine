#pragma once
#include "Includes/MeshletGeometry.slang"

// Mesh-shader version of the meshlet geometry path: ONE workgroup per meshlet. Shares all geometry with
// MeshletGeometry.slang.
//
// DEPTH-ONLY. The renderer binds this from exactly one place -- BindShadowBatchPipeline -- which always
// sets EMeshPass::Shadow, so EPASS_BASE is unreachable here: the forward base pass now runs through
// VisBuffer + DeferredMaterial, and MeshletVertex.slang keeps the base path for translucency. That is why
// the output is FDepthOnlyVertexOutput and not FBaseVertexOutput: carrying the full interpolant struct
// reserved 124B per vertex of mesh output storage for a pass whose only consumer reads SV_Position, and
// that reservation is what held cascade occupancy at 2.0 warps/cycle. See FDepthOnlyVertexOutput.
//
// The vertex load and world transform below still run in full, because the material vertex graph feeding
// World Position Offset may read the normal, tangent, UV or color. Only the OUTPUT is narrowed.
//
// Feeding contract (renderer, per CmdDrawMeshTasksIndirect):
//   - groupCountX     = surviving meshlets for this (view, material batch)
//   - Pass().MeshletBase = global MeshletDrawList offset for this draw -- the VS path's FirstInstance,
//     since mesh dispatch has no firstInstance. Workgroup meshlet = MeshletDrawList[MeshletBase + GroupID].
//   - Pass().ShadowDataIndex / ViewIndex are read only on the EPass == Shadow branch.

[vk::constant_id(0)]
const uint EPass = 0;

static const uint EPASS_BASE   = 0u;
static const uint EPASS_DEPTH  = 1u;
static const uint EPASS_SHADOW = 2u;

// One CmdDrawMeshTasksIndirect covers a whole batch; SV_DrawIndex selects the sub-draw. The per-draw meshlet
// base comes from ViewDrawOffsets, the same GPU-built array SeedIndirectArgs seeds FirstInstance from (so the
// mesh and VS paths consume the same value). It used to be hand-indexed out of FDrawIndirectArguments as raw
// uints ([arg*4 + 3]), which meant the mesh-task path reached into a struct belonging to the graphics
// indirect path and read a cacheline other lanes were atomically incrementing (InstanceCount is element 1 of
// the same 16 bytes).
struct FMeshPassArgs
{
    uint* ViewDrawOffsets; // per-(view, draw) base into MeshletDrawList()
    uint  ArgBase;         // first arg index for this batch (CullViewIndex*NumDrawsPerView + IndirectDrawOffset)
    int   ShadowDataIndex; // EPass == Shadow only
    int   ViewIndex;       // EPass == Shadow only (CSM cascade / point face / spot)
};
FMeshPassArgs* Pass() { return GetPassArgs<FMeshPassArgs>(); }

uint LoadMeshletBase(uint DrawIndex)
{
    return Pass().ViewDrawOffsets[Pass().ArgBase + DrawIndex];
}

// 128 threads covers a full meshlet in one pass: up to MESHLET_MAX_VERTICES (64) verts and
// MESHLET_MAX_TRIANGLES (124) prims, one of each per thread under the count guards.
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void MeshMain(
    uint3 GroupID   : SV_GroupID,
    uint  ThreadID  : SV_GroupIndex,
    uint  DrawIndex : SV_DrawIndex,
    out vertices FDepthOnlyVertexOutput Verts[MESHLET_MAX_VERTICES],
    out indices  uint3                  Tris[MESHLET_MAX_TRIANGLES])
{
    uint MeshletSlot = LoadMeshletBase(DrawIndex) + GroupID.x;
    FMeshletRef Ref  = ResolveMeshlet(MeshletSlot, EPass == EPASS_SHADOW);

    const uint VertCount = Ref.M.VertexCount;
    const uint TriCount  = Ref.M.TriangleCount;
    SetMeshOutputCounts(VertCount, TriCount);

    // One unique vertex per thread (no over-launch -- the mesh shader emits exactly VertexCount).
    if (ThreadID < VertCount)
    {
        FVertexData VertexData = LoadMeshletVertexData(Ref, ThreadID);
        FMeshletWorldVertex W  = TransformMeshletVertex(Ref, VertexData);

        // Same material vertex-graph local contract as MeshletVertex.slang
        // (the codegen alias preamble reads Inst / WorldPos / NormalWS / VertexData by these names).
        FGPUInstance* Inst   = Ref.Inst;   // pointer: FMeshletRef reaches the instance, see MeshletGeometry.slang
        float4x4 ModelMatrix = Ref.Inst.ModelMatrix;
        float4   WorldPos    = float4(W.WorldPos, 1.0);
        float3   NormalWS    = W.NormalWS;
        float3   TangentWS   = W.TangentWS.xyz;
        float    TangentSignWS = W.TangentWS.w;   // handedness; the graph's WorldTangent alias needs it

        FMaterialVertexInputs Material;
        $MATERIAL_VERTEX_INPUTS
        WorldPos.xyz += Material.WorldPositionOffset;

        const float3 FinalWorldPos = WorldPos.xyz;

        FDepthOnlyVertexOutput O;
        if (EPass == EPASS_SHADOW)
        {
            O.Position = mul(GetShadowViewProjection(Pass().ShadowDataIndex, (uint)Pass().ViewIndex), float4(FinalWorldPos, 1.0));
        }
        else // EPASS_DEPTH -- identical path to the base pass's SV_Position for the equal-depth test.
        {
            O.Position = CameraClipPosition(CameraViewPosition(FinalWorldPos));
        }
        Verts[ThreadID] = O;
    }

    // One triangle per thread; UnpackMeshletTriangle returns the 3 local vertex indices.
    if (ThreadID < TriCount)
    {
        Tris[ThreadID] = UnpackMeshletTriangleSafe(Ref, ThreadID);
    }
}
