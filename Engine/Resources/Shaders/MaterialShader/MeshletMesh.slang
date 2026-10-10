#pragma once
#include "Includes/MeshletGeometry.slang"

// Compiled per output type via MESHLET_MESH_BASE, since a spec constant cannot change output types.

// Masked shadow casters carry what their opacity graph reads, but rastered from the light rather than the camera.
#if defined(MESHLET_MESH_BASE)
typedef FBaseVertexOutput      FMeshVertexOutput;
#elif defined(MESHLET_MESH_MASKED_SHADOW)
typedef FMaskedVertexOutput    FMeshVertexOutput;
#else
typedef FDepthOnlyVertexOutput FMeshVertexOutput;
#endif


// Rejected in hardware rather than compacted, like FVisPrimitive in MeshletVisBuffer.slang.
struct FMeshPrimitiveOut
{
    bool CullPrimitive : SV_CullPrimitive;
#ifndef MESHLET_MESH_BASE
    // A multi-view draw sets one viewport and scissor per view on its atlas tile, in draw order.
    uint ViewportIndex : SV_ViewportArrayIndex;
#endif
};

// Kept outside the loop because the token expands raw statements that would otherwise be emitted per iteration.
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
    uint MeshletSlot;
    FMeshletRef Ref = ResolveMeshletForWorkgroup(DrawIndex, GroupID.x, MeshletSlot);

    const uint VertCount = Ref.M.VertexCount;
    const uint TriCount  = Ref.M.TriangleCount;

    // Set first so output allocation overlaps the vertex phase rather than stalling it.
    SetMeshOutputCounts(VertCount, TriCount);

#ifndef MESHLET_MESH_BASE
    const FShadowRasterView ShadowView = ShadowRasterViewForDraw(DrawIndex);
    const uint ShadowViewport = IsMultiViewShadowDraw() ? DrawIndex : 0u;
#endif

    // Kept only for ShuffleMeshletClip, which reads it from the owning lane's registers.
    float4 Clip[MESHLET_VERT_ITERS];

    [ForceUnroll]
    for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
    {
        Clip[v] = float4(0.0, 0.0, 0.0, 0.0);
    }

    // Workgroup-uniform, and needed because an unresolved meshlet leaves Ref.Inst a null base.
    if (VertCount > 0u)
    {
        // Issued before the vertex fetch so the loads overlap rather than serialize.
        const float4x4 ModelMatrix  = Ref.ModelMatrix;
        const float3x3 NormalMatrix = CalculateNormalMatrix(ModelMatrix);
#if defined(MESHLET_MESH_BASE) || defined(MESHLET_MESH_MASKED_SHADOW)
        const float4x4 CameraView   = GetCameraView();
        const float4x4 CameraProj   = GetCameraProjection();
#endif
#ifdef MESHLET_MESH_MASKED_SHADOW
        const uint     MaterialIndex  = Ref.Inst.MaterialIndex;
#endif
#ifndef MESHLET_MESH_BASE
        const float4x4 ShadowViewProj = GetShadowViewProjection(ShadowView.ShadowDataIndex, (uint)ShadowView.ViewIndex);
#endif

        [ForceUnroll]
        for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
        {
            // The affine export index and clamped load index must differ, see MeshletVisBuffer.slang.
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
            FMeshVertexOutput VOut = MakeBaseVertexOutput(Ref, W, FinalWorldPos, CameraView, CameraProj);
            Clip[v] = VOut.Position;   // taken from the output so the two cannot drift
#elif defined(MESHLET_MESH_MASKED_SHADOW)
            // Interpolants use the camera basis for the opacity graph, and only the clip position is the light's.
            FMeshVertexOutput VOut = MakeMaskedVertexOutput(Ref, W, FinalWorldPos, CameraView, CameraProj, MaterialIndex);
            Clip[v] = mul(ShadowViewProj, float4(FinalWorldPos, 1.0));
            VOut.Position = Clip[v];
#else
            FMeshVertexOutput VOut = (FMeshVertexOutput)0;
            Clip[v] = mul(ShadowViewProj, float4(FinalWorldPos, 1.0));
            VOut.Position = Clip[v];
#endif

            if (StoreSlot < VertCount)
            {
                Verts[StoreSlot] = VOut;
            }
        }
    }

    // Clip positions are in this pass's space, so the reject matches the rastered volume and winding.
#ifdef MESHLET_MESH_BASE
    const float2 ViewportSize = float2(MeshletPass().ViewportW, MeshletPass().ViewportH);
#else
    const float2 ViewportSize = ShadowView.TileSize;
#endif

    // Dead triangles carry SV_CullPrimitive, so there is no compaction. The clamp below needs a non-zero TriCount.
    if (TriCount > 0u)
    {
        [ForceUnroll]
        for (uint i = 0; i < MESHLET_TRI_ITERS; ++i)
        {
            // The clamped index keeps every lane active for the shuffle, the affine index is for the export.
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
#ifndef MESHLET_MESH_BASE
                Prims[StoreT].ViewportIndex = ShadowViewport;
#endif
            }
        }
    }
}
