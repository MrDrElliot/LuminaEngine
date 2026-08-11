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

    // FIRST, ahead of the vertex fetch. This is what reserves output storage in the geometry engine, so
    // issuing it here overlaps that allocation with the whole vertex phase instead of stalling behind it.
    // Both counts come straight off the meshlet header -- nothing below can change them. (It used to sit
    // after the vertex phase because primitives were COMPACTED and the count was not known until then;
    // SV_CullPrimitive replaced compaction and that constraint went with it.)
    SetMeshOutputCounts(VertCount, TriCount);

    // The only thing the vertex phase carries in registers now: ShuffleMeshletClip reads it out of the
    // OWNING LANE, so it is indexed by lane, not by vertex slot. The vertex output itself is exported
    // as it is produced.
    float4 Clip[MESHLET_VERT_ITERS];

    [ForceUnroll]
    for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
    {
        Clip[v] = float4(0.0, 0.0, 0.0, 0.0);
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
            // TWO indices on purpose. The EXPORT index must stay a static affine function of the lane
            // (ThreadID + v*GroupSize) or the compiler cannot prove lane i owns vertex i and pushes every
            // export through LDS -- a clamp against a runtime count is not provable. RDNA2 already pays
            // that for v>0 (one export per thread, the rest carried by hidden threads), and RDNA3's
            // wave-offset export needs the same i / i+N shape to kick in. The LOAD still clamps: surplus
            // lanes redundantly reprocess the last vertex instead of sitting masked, and every lane must
            // stay active for ShuffleMeshletClip below.
            const uint StoreSlot = ThreadID + v * MESHLET_MESH_GROUP_SIZE;
            const uint LoadSlot  = min(StoreSlot, VertCount - 1u);

            FVertexData VertexData = LoadMeshletVertexData(Ref, LoadSlot);
            FMeshletWorldVertex W  = TransformMeshletVertex(VertexData, ModelMatrix, NormalMatrix);

            float4 WorldPos = float4(W.WorldPos, 1.0);
            const FMaterialVertexInputs Material =
                EvalMaterialVertex(Ref.Inst, ModelMatrix, VertexData, WorldPos,
                                   W.NormalWS, W.TangentWS.xyz, W.TangentWS.w);
            WorldPos.xyz += Material.WorldPositionOffset;

#ifdef VISBUFFER_MASKED_GEOM
            FVisVertexOutput VOut = MakeBaseVertexOutput(Ref, W, WorldPos.xyz, CameraView, CameraProj);
            Clip[v] = VOut.Position;   // the same two muls; taken from the output so they cannot drift
#else
            FVisVertexOutput VOut = (FVisVertexOutput)0;
            Clip[v]       = CameraClipPosition(CameraViewPosition(WorldPos.xyz, CameraView), CameraProj);
            VOut.Position = Clip[v];
#endif

            // Dropping the clamped lanes' store is what keeps the index provable; they only ever
            // duplicated the last vertex anyway. Clip[] is still written by EVERY lane -- the shuffle
            // reads it by lane, and for any V < VertCount the owning lane holds the unclamped value.
            if (StoreSlot < VertCount)
            {
                Verts[StoreSlot] = VOut;
            }
        }
    }

    // Triangle phase. Every triangle is declared and the dead ones carry SV_CullPrimitive, so there is no
    // compaction pass, no shared bitmask and no second barrier.
    // Workgroup-UNIFORM, so no lane diverges on it; TriCount - 1u below needs the non-zero case.
    if (TriCount > 0u)
    {
        [ForceUnroll]
        for (uint i = 0; i < MESHLET_TRI_ITERS; ++i)
        {
            // Same split as the vertex phase: the cull test runs on the CLAMPED index so every lane stays
            // active for ShuffleMeshletClip's WaveReadLaneAt, while the export index stays affine. Where
            // the store survives, StoreT < TriCount, so t == StoreT and Idx is that triangle.
            const uint  StoreT = ThreadID + i * MESHLET_MESH_GROUP_SIZE;
            const uint  t      = min(StoreT, TriCount - 1u);
            const uint3 Idx    = UnpackMeshletTriangleSafe(Ref, t);

            const bool bCull = IsDegenerateTriangleIndices(Idx)
                             | IsMeshletTriangleCulled(ShuffleMeshletClip(Clip, Idx.x),
                                                       ShuffleMeshletClip(Clip, Idx.y),
                                                       ShuffleMeshletClip(Clip, Idx.z),
                                                       float2(GetScreenSize()));

            if (StoreT < TriCount)
            {
                // The slot IS the source triangle index now, so VisID needs no separate mapping: the
                // deferred pass re-fetches the triangle out of the meshlet's own array.
                Tris[StoreT]                = Idx;
                Prims[StoreT].VisID         = ((MeshletSlot << 7u) | (StoreT & 0x7Fu)) + 1u;
                Prims[StoreT].CullPrimitive = bCull;
            }
        }
    }
}
