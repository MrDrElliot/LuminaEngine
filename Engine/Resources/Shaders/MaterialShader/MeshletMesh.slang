#pragma once
#include "Includes/MeshletGeometry.slang"

// Mesh-shader version of the meshlet geometry path: ONE workgroup per meshlet. It shares all geometry
// with MeshletGeometry.slang and emits the SAME FBaseVertexOutput as MeshletVertex.slang, so the
// fragment shader (BasePixelPass) is identical across both paths. Pass is chosen by the EPass spec
// constant, exactly mirroring MeshletVertex.slang.
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

// One CmdDrawMeshTasksIndirect covers a whole batch; SV_DrawIndex selects the sub-draw and the per-draw
// meshlet base is the cull's FDrawIndirectArguments.FirstInstance (raw uints, 4/arg, FirstInstance = element 3).
struct FMeshPassArgs
{
    uint* IndirectArgs;    // FDrawIndirectArguments[] as raw uints; [arg*4 + 3] = FirstInstance = MeshletBase
    uint  ArgBase;         // first arg index for this batch (CullViewIndex*NumDrawsPerView + IndirectDrawOffset)
    int   ShadowDataIndex; // EPass == Shadow only
    int   ViewIndex;       // EPass == Shadow only (CSM cascade / point face / spot)
};
FMeshPassArgs* Pass() { return GetPassArgs<FMeshPassArgs>(); }

uint LoadMeshletBase(uint DrawIndex)
{
    return Pass().IndirectArgs[(Pass().ArgBase + DrawIndex) * 4u + 3u];
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
    out vertices FBaseVertexOutput Verts[MESHLET_MAX_VERTICES],
    out indices  uint3             Tris[MESHLET_MAX_TRIANGLES])
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
        FGPUInstance Inst    = Ref.Inst;
        float4x4 ModelMatrix = Ref.Inst.ModelMatrix;
        float4   WorldPos    = float4(W.WorldPos, 1.0);
        float3   NormalWS    = W.NormalWS;
        float3   TangentWS   = W.TangentWS.xyz;

        FMaterialVertexInputs Material;
        $MATERIAL_VERTEX_INPUTS
        WorldPos.xyz += Material.WorldPositionOffset;

        const float3 FinalWorldPos = WorldPos.xyz;

        FBaseVertexOutput O;
        if (EPass == EPASS_BASE)
        {
            O = MakeBaseVertexOutput(Ref, W, FinalWorldPos);
        }
        else
        {
            // Depth + Shadow: position only; interpolants zeroed (no fragment stage reads them).
            O = MakeDegenerateBaseVertexOutput();
            if (EPass == EPASS_SHADOW)
            {
                O.Position = mul(GetShadowViewProjection(Pass().ShadowDataIndex, (uint)Pass().ViewIndex), float4(FinalWorldPos, 1.0));
            }
            else // EPASS_DEPTH -- identical path to Base's SV_Position for the equal-depth test.
            {
                O.Position = CameraClipPosition(CameraViewPosition(FinalWorldPos));
            }
        }
        Verts[ThreadID] = O;
    }

    // One triangle per thread; UnpackMeshletTriangle returns the 3 local vertex indices.
    if (ThreadID < TriCount)
    {
        Tris[ThreadID] = UnpackMeshletTriangle(Ref, ThreadID);
    }
}
