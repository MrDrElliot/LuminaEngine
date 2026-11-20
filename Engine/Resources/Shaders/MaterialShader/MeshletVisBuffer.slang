#pragma once
#include "Includes/MeshletGeometry.slang"

// Compiled twice via VISBUFFER_MASKED_GEOM since the cases need different output types; VisID is per primitive, stored +1.

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

// What every vertex of one meshlet reads alike, loaded once per mesh workgroup and once per vertex on the vertex path.
struct FVisVertexContext
{
    float4x4 ModelMatrix;
    float3x3 NormalMatrix;
    float4x4 CameraView;
    float4x4 CameraProj;
#ifdef VISBUFFER_MASKED_GEOM
    uint     MaterialIndex;
#endif
    bool     bApplyWPO;
};

FVisVertexContext LoadVisVertexContext(FMeshletRef Ref)
{
    FVisVertexContext Ctx;
    Ctx.ModelMatrix   = Ref.ModelMatrix;
    Ctx.NormalMatrix  = CalculateNormalMatrix(Ctx.ModelMatrix);
    Ctx.CameraView    = GetCameraView();
    Ctx.CameraProj    = GetCameraProjection();
#ifdef VISBUFFER_MASKED_GEOM
    Ctx.MaterialIndex = Ref.Inst.MaterialIndex;
#endif
    Ctx.bApplyWPO     = Ref.Inst.bApplyWPO;
    return Ctx;
}

// Both entry points emit their vertices through this, so the mesh and vertex paths place geometry identically.
FVisVertexOutput VisBufferVertex(FMeshletRef Ref, uint LocalVertex, FVisVertexContext Ctx)
{
    FVertexData VertexData = LoadMeshletVertexData(Ref, LocalVertex);
    FMeshletWorldVertex W  = TransformMeshletVertex(VertexData, Ctx.ModelMatrix, Ctx.NormalMatrix);

    float4 WorldPos = float4(W.WorldPos, 1.0);
    if (Ctx.bApplyWPO)
    {
        const FMaterialVertexInputs Material =
            EvalMaterialVertex(Ref.Inst, Ctx.ModelMatrix, VertexData, WorldPos,
                               W.NormalWS, W.TangentWS.xyz, W.TangentWS.w);
        WorldPos.xyz += Material.WorldPositionOffset;
    }

#ifdef VISBUFFER_MASKED_GEOM
    return MakeMaskedVertexOutput(Ref, W, WorldPos.xyz, Ctx.CameraView, Ctx.CameraProj, Ctx.MaterialIndex);
#else
    FVisVertexOutput VOut = (FVisVertexOutput)0;
    VOut.Position = CameraClipPosition(CameraViewPosition(WorldPos.xyz, Ctx.CameraView), Ctx.CameraProj);
    return VOut;
#endif
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
        // Every workgroup-uniform load is issued before the vertex fetch so they are in flight together.
        const FVisVertexContext Ctx = LoadVisVertexContext(Ref);

        [ForceUnroll]
        for (uint v = 0; v < MESHLET_VERT_ITERS; ++v)
        {
            // The export index must stay affine in the lane or every export goes through LDS; the load clamps so all lanes stay active for the shuffle.
            const uint StoreSlot = ThreadID + v * MESHLET_MESH_GROUP_SIZE;
            const uint LoadSlot  = min(StoreSlot, VertCount - 1u);

            const FVisVertexOutput VOut = VisBufferVertex(Ref, LoadSlot, Ctx);
            Clip[v] = VOut.Position;

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

            // Only the camera's views have to fit the ID range, and a slot past it would alias the proxy bit.
            const bool bCull = MeshletSlot > VISBUFFER_MAX_MESHLET_SLOTS
                             | IsDegenerateTriangleIndices(Idx)
                             | IsMeshletTriangleCulled(ShuffleMeshletClip(Clip, Idx.x),
                                                       ShuffleMeshletClip(Clip, Idx.y),
                                                       ShuffleMeshletClip(Clip, Idx.z),
                                                       float2(GetScreenSize()));

            if (StoreT < TriCount)
            {
                // The slot is the source triangle index, so the deferred pass re-fetches it from the meshlet's own array.
                Tris[StoreT]                = Idx;
                Prims[StoreT].VisID         = PackVisID(MeshletSlot, StoreT);
                Prims[StoreT].CullPrimitive = bCull;
            }
        }
    }
}

struct FVisVertexStageOutput
{
    FVisVertexOutput     Vertex;
    nointerpolation uint VisID : VISID;
};

// The fallback without mesh shading, one instance per surviving meshlet, with an invalid corner left at the origin with no area.
[shader("vertex")]
FVisVertexStageOutput VertexMain(uint VertexIndex   : SV_VulkanVertexID,
                                 uint InstanceIndex : SV_VulkanInstanceID,
                                 uint DrawIndex     : SV_DrawIndex)
{
    const FMeshletCorner Corner = ResolveMeshletCorner(DrawIndex, InstanceIndex, VertexIndex);

    FVisVertexStageOutput Out = (FVisVertexStageOutput)0;
    Out.VisID = PackVisID(Corner.MeshletSlot, Corner.TriIdx);
    if (Corner.bValid && Corner.MeshletSlot <= VISBUFFER_MAX_MESHLET_SLOTS)
    {
        Out.Vertex = VisBufferVertex(Corner.Ref, Corner.LocalVertex, LoadVisVertexContext(Corner.Ref));
    }
    return Out;
}
