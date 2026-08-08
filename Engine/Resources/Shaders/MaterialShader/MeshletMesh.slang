#pragma once
#include "Includes/MeshletGeometry.slang"

// Mesh-shader version of the meshlet geometry path: ONE workgroup per meshlet. Shares all geometry with
// MeshletGeometry.slang.
//
// Compiled TWICE, selected by MESHLET_MESH_BASE, because the two cases need different OUTPUT TYPES and a
// spec constant cannot change one -- the same constraint that splits MeshletVisBuffer's masked variant:
//   - undefined (shadow): FDepthOnlyVertexOutput, position only. Carrying the full interpolant struct
//     reserved 124B per vertex of mesh output storage for a pass whose only consumer reads SV_Position,
//     and that reservation is what held cascade occupancy at 2.0 warps/cycle.
//   - defined (base): FBaseVertexOutput, for translucent and additive geometry, whose pixel shader shades
//     forward and needs the whole interpolant set.
//
// The vertex load and world transform run in full either way, because the material vertex graph feeding
// World Position Offset may read the normal, tangent, UV or color. Only the OUTPUT is narrowed.
//
// The meshlet arrives through the task stage's payload; FMeshletPassArgs (MeshletGeometry.slang) is the
// push block both stages share. ShadowDataIndex / ViewIndex are read on the shadow variant only, and
// ViewportW/H is the viewport being rastered (for a shadow pass the atlas TILE) for the small-primitive
// reject.

#ifdef MESHLET_MESH_BASE
typedef FBaseVertexOutput      FMeshVertexOutput;
#else
typedef FDepthOnlyVertexOutput FMeshVertexOutput;
#endif


// Hardware-side reject; see FVisPrimitive in MeshletVisBuffer.slang for why this replaced compaction.
struct FMeshPrimitiveOut
{
    bool CullPrimitive : SV_CullPrimitive;
};

// The material vertex graph, emitted ONCE. The token substitutes raw statements that reach these by name,
// so they arrive as parameters: left inside the vertex loop, the whole graph body would be emitted per
// iteration.
FMaterialVertexInputs EvalMaterialVertex(FGPUInstance* Inst, float4x4 ModelMatrix, FVertexData VertexData,
                                         float4 WorldPos, float3 NormalWS, float3 TangentWS,
                                         float TangentSignWS)
{
    FMaterialVertexInputs Material;
    $MATERIAL_VERTEX_INPUTS
    return Material;
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MESHLET_MESH_GROUP_SIZE, 1, 1)]
void MeshMain(
    uint3 GroupID   : SV_GroupID,
    uint  ThreadID  : SV_GroupIndex,
    in payload   FVisTaskPayload    TaskPayload,
    out vertices FMeshVertexOutput  Verts[MESHLET_MAX_VERTICES],
    out indices    uint3             Tris[MESHLET_MAX_TRIANGLES],
    out primitives FMeshPrimitiveOut Prims[MESHLET_MAX_TRIANGLES])
{
    // Compacted survivor index, not a draw-list offset -- see MeshletVisBuffer.slang.
    FMeshletRef Ref = MakeMeshletRefFromTask(TaskPayload, GroupID.x);

    const uint VertCount = Ref.M.VertexCount;
    const uint TriCount  = Ref.M.TriangleCount;

    // Held in registers until the compacted primitive count is known; see MeshletVisBuffer.slang.
    FMeshVertexOutput VOut[MESHLET_VERT_ITERS];
    float4            Clip[MESHLET_VERT_ITERS];
    uint              VertSlot[MESHLET_VERT_ITERS];

    [ForceUnroll]
    for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
    {
        VOut[v]     = (FMeshVertexOutput)0;
        Clip[v]     = float4(0.0, 0.0, 0.0, 0.0);
        VertSlot[v] = 0u;
    }

    // Workgroup-UNIFORM, so this branch is free and no lane diverges on it. It stays because an
    // unresolved meshlet leaves Instances() null, which would make Ref.Inst a null base below.
    if (VertCount > 0u)
    {
        // Issued before the vertex fetch so the loads overlap rather than serialize; see the same
        // block in MeshletVisBuffer.slang.
        const float4x4 ModelMatrix  = Ref.Inst.ModelMatrix;
        const float3x3 NormalMatrix = CalculateNormalMatrix(ModelMatrix);

        [ForceUnroll]
        for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
        {
            // Clamped rather than guarded: surplus lanes redundantly reprocess the last vertex instead
            // of sitting masked. They were not saving anything behind a per-lane bounds test (the wave
            // issues the body regardless), and the duplicate store writes an identical value.
            VertSlot[v] = min(ThreadID + v * MESHLET_MESH_GROUP_SIZE, VertCount - 1u);

            FVertexData VertexData = LoadMeshletVertexData(Ref, VertSlot[v]);
            FMeshletWorldVertex W  = TransformMeshletVertex(VertexData, ModelMatrix, NormalMatrix);

            float4 WorldPos = float4(W.WorldPos, 1.0);
            const FMaterialVertexInputs Material =
                EvalMaterialVertex(Ref.Inst, ModelMatrix, VertexData, WorldPos,
                                   W.NormalWS, W.TangentWS.xyz, W.TangentWS.w);
            WorldPos.xyz += Material.WorldPositionOffset;

            const float3 FinalWorldPos = WorldPos.xyz;

#ifdef MESHLET_MESH_BASE
            VOut[v] = MakeBaseVertexOutput(Ref, W, FinalWorldPos, GetCameraView(), GetCameraProjection());
            Clip[v] = VOut[v].Position;   // taken from the output so the two cannot drift
#else
            Clip[v] = mul(GetShadowViewProjection(MeshletPass().ShadowDataIndex, (uint)MeshletPass().ViewIndex),
                          float4(FinalWorldPos, 1.0));
#endif
        }
    }

    // Triangle cull. The clip positions above are in THIS pass's space (the shadow VP for a shadow view,
    // the camera VP for depth), so the frustum reject is against the volume actually being rastered and
    // the winding is the one the hardware will see.
    const float2 ViewportSize = float2(MeshletPass().ViewportW, MeshletPass().ViewportH);

    // Every triangle is declared and the dead ones carry SV_CullPrimitive, so there is no compaction
    // pass, no shared bitmask and no second barrier.
    SetMeshOutputCounts(VertCount, TriCount);

    if (VertCount > 0u)
    {
        [ForceUnroll]
        for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
        {
            VOut[v].Position   = Clip[v];
            Verts[VertSlot[v]] = VOut[v];
        }
    }

    // Workgroup-UNIFORM, so no lane diverges on it; TriCount - 1u below needs the non-zero case.
    if (TriCount > 0u)
    {
        [ForceUnroll]
        for (uint i = 0; i < MESHLET_TRI_ITERS; ++i)
        {
            const uint  t   = min(ThreadID + i * MESHLET_MESH_GROUP_SIZE, TriCount - 1u);
            const uint3 Idx = UnpackMeshletTriangleSafe(Ref, t);

            Tris[t]                = Idx;
            Prims[t].CullPrimitive = IsDegenerateTriangleIndices(Idx)
                                   | IsMeshletTriangleCulled(ShuffleMeshletClip(Clip, Idx.x),
                                                             ShuffleMeshletClip(Clip, Idx.y),
                                                             ShuffleMeshletClip(Clip, Idx.z), ViewportSize);
        }
    }
}
