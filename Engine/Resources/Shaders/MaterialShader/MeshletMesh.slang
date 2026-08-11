#pragma once
#include "Includes/MeshletGeometry.slang"

// ONE workgroup per meshlet. Compiled TWICE via MESHLET_MESH_BASE: the two cases need different OUTPUT
// TYPES, which a spec constant cannot change. The load and transform run in full either way, for WPO.

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

// The material vertex graph, emitted ONCE outside the loop: the token substitutes raw statements that
// reach these by name, so left inside, the whole body would be emitted per iteration.
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
    out vertices FMeshVertexOutput  Verts[MESHLET_MAX_VERTICES],
    out indices    uint3             Tris[MESHLET_MAX_TRIANGLES],
    out primitives FMeshPrimitiveOut Prims[MESHLET_MAX_TRIANGLES])
{
    // One workgroup per survivor MeshletCull.slang compacted into this bucket's slice.
    uint MeshletSlot;
    FMeshletRef Ref = ResolveMeshletForWorkgroup(DrawIndex, GroupID.x, MeshletSlot);

    const uint VertCount = Ref.M.VertexCount;
    const uint TriCount  = Ref.M.TriangleCount;

    // FIRST, ahead of the vertex fetch, so the geometry engine's output allocation overlaps the vertex
    // phase rather than stalling it; see MeshletVisBuffer.slang for why it used to sit at the bottom.
    SetMeshOutputCounts(VertCount, TriCount);

    // Only the clip position survives the vertex phase, and only because ShuffleMeshletClip reads it out
    // of the owning LANE's registers; see MeshletVisBuffer.slang.
    float4 Clip[MESHLET_VERT_ITERS];

    [ForceUnroll]
    for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
    {
        Clip[v] = float4(0.0, 0.0, 0.0, 0.0);
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
            // Affine EXPORT index, clamped LOAD index -- see MeshletVisBuffer.slang for why the two must
            // not be the same value.
            const uint StoreSlot = ThreadID + v * MESHLET_MESH_GROUP_SIZE;
            const uint LoadSlot  = min(StoreSlot, VertCount - 1u);

            FVertexData VertexData = LoadMeshletVertexData(Ref, LoadSlot);
            FMeshletWorldVertex W  = TransformMeshletVertex(VertexData, ModelMatrix, NormalMatrix);

            float4 WorldPos = float4(W.WorldPos, 1.0);
            const FMaterialVertexInputs Material =
                EvalMaterialVertex(Ref.Inst, ModelMatrix, VertexData, WorldPos,
                                   W.NormalWS, W.TangentWS.xyz, W.TangentWS.w);
            WorldPos.xyz += Material.WorldPositionOffset;

            const float3 FinalWorldPos = WorldPos.xyz;

#ifdef MESHLET_MESH_BASE
            FMeshVertexOutput VOut = MakeBaseVertexOutput(Ref, W, FinalWorldPos, GetCameraView(), GetCameraProjection());
            Clip[v] = VOut.Position;   // taken from the output so the two cannot drift
#else
            FMeshVertexOutput VOut = (FMeshVertexOutput)0;
            Clip[v] = mul(GetShadowViewProjection(MeshletPass().ShadowDataIndex, (uint)MeshletPass().ViewIndex),
                          float4(FinalWorldPos, 1.0));
            VOut.Position = Clip[v];
#endif

            if (StoreSlot < VertCount)
            {
                Verts[StoreSlot] = VOut;
            }
        }
    }

    // The clip positions above are in THIS pass's space, so the frustum reject is against the volume
    // actually being rastered and the winding is the one the hardware will see.
    const float2 ViewportSize = float2(MeshletPass().ViewportW, MeshletPass().ViewportH);

    // Every triangle is declared and the dead ones carry SV_CullPrimitive, so there is no compaction
    // pass, no shared bitmask and no second barrier.
    // Workgroup-UNIFORM, so no lane diverges on it; TriCount - 1u below needs the non-zero case.
    if (TriCount > 0u)
    {
        [ForceUnroll]
        for (uint i = 0; i < MESHLET_TRI_ITERS; ++i)
        {
            // Clamped index for the test (every lane must stay active for the shuffle), affine index for
            // the export; where the store survives the two are equal.
            const uint  StoreT = ThreadID + i * MESHLET_MESH_GROUP_SIZE;
            const uint  t      = min(StoreT, TriCount - 1u);
            const uint3 Idx    = UnpackMeshletTriangleSafe(Ref, t);

            const bool bCull = IsDegenerateTriangleIndices(Idx)
                             | IsMeshletTriangleCulled(ShuffleMeshletClip(Clip, Idx.x),
                                                       ShuffleMeshletClip(Clip, Idx.y),
                                                       ShuffleMeshletClip(Clip, Idx.z), ViewportSize);

            if (StoreT < TriCount)
            {
                Tris[StoreT]                = Idx;
                Prims[StoreT].CullPrimitive = bCull;
            }
        }
    }
}
