#pragma once
#include "Includes/MeshletGeometry.slang"

// Compiled twice via VISBUFFER_MASKED_GEOM since the cases need different output types; VisID is per primitive, stored +1.

// SV_DrawIndex selects the sub-draw, whose base comes from the same ViewDrawOffsets the task stage appended through.

struct FVisPrimitive
{
    uint VisID          : SV_PrimitiveID;
    // Flagging dead triangles beats compacting, since output storage is reserved from the declared size either way.
    bool CullPrimitive  : SV_CullPrimitive;
};

// Masked carries what its opacity graph reads, and opaque only a clip position since VisID is per primitive.
#ifdef VISBUFFER_MASKED_GEOM
typedef FMaskedVertexOutput    FVisVertexOutput;
#else
typedef FDepthOnlyVertexOutput FVisVertexOutput;
#endif

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
    // The slot is the draw-list index, which VisID packs and DeferredMaterial resolves back through.
    uint MeshletSlot;
    FMeshletRef Ref = ResolveMeshletForWorkgroup(DrawIndex, GroupID.x, MeshletSlot);

    const uint VertCount = Ref.M.VertexCount;
    const uint TriCount  = Ref.M.TriangleCount;

    // Issued ahead of the vertex fetch so the output allocation overlaps the vertex phase.
    SetMeshOutputCounts(VertCount, TriCount);

    // Indexed by lane, not vertex slot, since ShuffleMeshletClip reads it from the owning lane.
    float4 Clip[MESHLET_VERT_ITERS];

    [ForceUnroll]
    for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
    {
        Clip[v] = float4(0.0, 0.0, 0.0, 0.0);
    }

    // Workgroup-uniform; an unresolved meshlet leaves VertCount 0 and Ref.Inst possibly null.
    if (VertCount > 0u)
    {
        // Every workgroup-uniform matrix load is issued before the vertex fetch so all four are in flight together.
        const float4x4 ModelMatrix  = Ref.ModelMatrix;
        const float3x3 NormalMatrix = CalculateNormalMatrix(ModelMatrix);
        const float4x4 CameraView   = GetCameraView();
        const float4x4 CameraProj   = GetCameraProjection();
#ifdef VISBUFFER_MASKED_GEOM
        const uint     MaterialIndex = Ref.Inst.MaterialIndex;
#endif

        [ForceUnroll]
        for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
        {
            // The export index must stay affine in the lane or every export goes through LDS; the load clamps so all lanes stay active for the shuffle.
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
            FVisVertexOutput VOut = MakeMaskedVertexOutput(Ref, W, WorldPos.xyz, CameraView, CameraProj, MaterialIndex);
            Clip[v] = VOut.Position;   // taken from the output so it cannot drift from the exported position
#else
            FVisVertexOutput VOut = (FVisVertexOutput)0;
            Clip[v]       = CameraClipPosition(CameraViewPosition(WorldPos.xyz, CameraView), CameraProj);
            VOut.Position = Clip[v];
#endif

            // Clamped lanes skip the store but still write Clip[], which the shuffle reads by lane.
            if (StoreSlot < VertCount)
            {
                Verts[StoreSlot] = VOut;
            }
        }
    }

    // Workgroup-uniform, and the clamp below needs a non-zero TriCount.
    if (TriCount > 0u)
    {
        [ForceUnroll]
        for (uint i = 0; i < MESHLET_TRI_ITERS; ++i)
        {
            // The cull test uses the clamped index so every lane stays active for WaveReadLaneAt, while the export index stays affine.
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
                // The slot is the source triangle index, so the deferred pass re-fetches it from the meshlet's own array.
                Tris[StoreT]                = Idx;
                Prims[StoreT].VisID         = ((MeshletSlot << 7u) | (StoreT & 0x7Fu)) + 1u;
                Prims[StoreT].CullPrimitive = bCull;
            }
        }
    }
}
