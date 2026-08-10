#pragma once
#include "Includes/MeshletGeometry.slang"

// VisBuffer geometry MESH shader, compiled TWICE via VISBUFFER_MASKED_GEOM: the two cases need
// different OUTPUT TYPES, which a spec constant cannot change. VisID is per-PRIMITIVE, stored +1.

// One CmdDrawMeshTasksIndirect covers a batch; SV_DrawIndex selects the sub-draw. The per-draw base
// comes from ViewDrawOffsets, the same GPU-built array the task stage appended through.

struct FVisPrimitive
{
    uint VisID          : SV_PrimitiveID;
    // Declaring every triangle and flagging the dead ones beats compacting: output storage is reserved
    // from the DECLARED array size either way, so compaction only ever cost.
    bool CullPrimitive  : SV_CullPrimitive;
};

// The per-vertex output type this variant declares. Masked needs the interpolant set; opaque needs a clip
// position and nothing else (VisID is per-PRIMITIVE, below).
#ifdef VISBUFFER_MASKED_GEOM
typedef FBaseVertexOutput      FVisVertexOutput;
#else
typedef FDepthOnlyVertexOutput FVisVertexOutput;
#endif


// Sized to the VERTEX phase (64), not the triangle cap: the vertex phase is the expensive one, so all
// 64 lanes must do it. The material vertex token is emitted ONCE, outside the loop.
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
    uint  DrawIndex : SV_DrawIndex,
    out vertices   FVisVertexOutput Verts[MESHLET_MAX_VERTICES],
    out indices    uint3            Tris[MESHLET_MAX_TRIANGLES],
    out primitives FVisPrimitive    Prims[MESHLET_MAX_TRIANGLES])
{
    // One workgroup per survivor MeshletCull.slang compacted into this bucket's slice. The slot is the
    // draw-list index itself now, which is what VisID packs and DeferredMaterial resolves back through.
    uint MeshletSlot;
    FMeshletRef Ref = ResolveMeshletForWorkgroup(DrawIndex, GroupID.x, MeshletSlot);

    const uint VertCount = Ref.M.VertexCount;
    const uint TriCount  = Ref.M.TriangleCount;

    // Vertex phase. The output is NOT written yet: SetMeshOutputCounts must publish the compacted
    // primitive count first, so the result is held in this lane's registers.
    FVisVertexOutput VOut[MESHLET_VERT_ITERS];
    float4           Clip[MESHLET_VERT_ITERS];
    uint             VertSlot[MESHLET_VERT_ITERS];

    [ForceUnroll]
    for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
    {
        VOut[v]     = (FVisVertexOutput)0;
        Clip[v]     = float4(0.0, 0.0, 0.0, 0.0);
        VertSlot[v] = 0u;
    }

    // Workgroup-UNIFORM, so this branch is free and no lane diverges on it. It stays because a meshlet
    // that failed to resolve leaves VertCount at 0 and Instances() may be null, making Ref.Inst null+0.
    if (VertCount > 0u)
    {
        // Every workgroup-uniform matrix load is issued HERE, before the vertex fetch, so all four requests
        // are in flight together. Strung end to end, this pass sat at 93% long-scoreboard.
        const float4x4 ModelMatrix  = Ref.Inst.ModelMatrix;
        const float3x3 NormalMatrix = CalculateNormalMatrix(ModelMatrix);
        const float4x4 CameraView   = GetCameraView();
        const float4x4 CameraProj   = GetCameraProjection();

        [ForceUnroll]
        for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
        {
            // Clamped rather than guarded: surplus lanes redundantly reprocess the last vertex. A per-lane bounds
            // test saved nothing (the wave issues the body regardless) and the duplicate store is identical.
            VertSlot[v] = min(ThreadID + v * MESHLET_MESH_GROUP_SIZE, VertCount - 1u);

            FVertexData VertexData = LoadMeshletVertexData(Ref, VertSlot[v]);
            FMeshletWorldVertex W  = TransformMeshletVertex(VertexData, ModelMatrix, NormalMatrix);

            float4 WorldPos = float4(W.WorldPos, 1.0);
            const FMaterialVertexInputs Material =
                EvalMaterialVertex(Ref.Inst, ModelMatrix, VertexData, WorldPos,
                                   W.NormalWS, W.TangentWS.xyz, W.TangentWS.w);
            WorldPos.xyz += Material.WorldPositionOffset;

#ifdef VISBUFFER_MASKED_GEOM
            VOut[v] = MakeBaseVertexOutput(Ref, W, WorldPos.xyz, CameraView, CameraProj);
            Clip[v] = VOut[v].Position;   // the same two muls; taken from the output so they cannot drift
#else
            Clip[v]          = CameraClipPosition(CameraViewPosition(WorldPos.xyz, CameraView), CameraProj);
            VOut[v].Position = Clip[v];
#endif
        }
    }

    // Triangle phase. Every triangle is declared and the dead ones carry SV_CullPrimitive, so there is no
    // compaction pass, no shared bitmask and no second barrier.
    SetMeshOutputCounts(VertCount, TriCount);

    if (VertCount > 0u)
    {
        [ForceUnroll]
        for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
        {
            Verts[VertSlot[v]] = VOut[v];
        }
    }

    // Workgroup-UNIFORM, so no lane diverges on it; TriCount - 1u below needs the non-zero case.
    if (TriCount > 0u)
    {
        [ForceUnroll]
        for (uint i = 0; i < MESHLET_TRI_ITERS; ++i)
        {
            // Clamped for the same reason as the vertex phase: surplus lanes redundantly re-test the last
            // triangle and write it identically, rather than sitting masked behind a bounds branch.
            const uint  t   = min(ThreadID + i * MESHLET_MESH_GROUP_SIZE, TriCount - 1u);
            const uint3 Idx = UnpackMeshletTriangleSafe(Ref, t);

            const bool bCull = IsDegenerateTriangleIndices(Idx)
                             | IsMeshletTriangleCulled(ShuffleMeshletClip(Clip, Idx.x),
                                                       ShuffleMeshletClip(Clip, Idx.y),
                                                       ShuffleMeshletClip(Clip, Idx.z),
                                                       float2(GetScreenSize()));

            // The slot IS the source triangle index now, so VisID needs no separate mapping: the deferred
            // pass re-fetches the triangle out of the meshlet's own array.
            Tris[t]             = Idx;
            Prims[t].VisID      = ((MeshletSlot << 7u) | (t & 0x7Fu)) + 1u;
            Prims[t].CullPrimitive = bCull;
        }
    }
}
