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

// See MeshletVisBuffer.slang: lane i owns vertex i, so only the clip position crosses lanes.
groupshared float4 gsClipPos[MESHLET_MAX_VERTICES];
groupshared uint   gsTriBits[MESHLET_TRI_WORDS];

// 64 threads = MESHLET_MAX_VERTICES, so the expensive vertex phase runs at full lane utilization; the
// cheap triangle phase strides those 64 lanes over the 124-triangle cap (2 iterations). This used to be
// 128 threads with one triangle per lane, which left lanes 64..127 idle through the whole vertex phase --
// the same shape MeshletVisBuffer.slang was already fixed for.
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void MeshMain(
    uint3 GroupID   : SV_GroupID,
    uint  ThreadID  : SV_GroupIndex,
    in payload   FVisTaskPayload    TaskPayload,
    out vertices FMeshVertexOutput  Verts[MESHLET_MAX_VERTICES],
    out indices  uint3              Tris[MESHLET_MAX_TRIANGLES])
{
    // Compacted survivor index, not a draw-list offset -- see MeshletVisBuffer.slang.
    FMeshletRef Ref = MakeMeshletRefFromTask(TaskPayload, GroupID.x);

    const uint VertCount = Ref.M.VertexCount;
    const uint TriCount  = Ref.M.TriangleCount;

    if (ThreadID < MESHLET_TRI_WORDS)
    {
        gsTriBits[ThreadID] = 0u;
    }

    // One unique vertex per thread (no over-launch -- the mesh shader emits exactly VertexCount). The
    // output is held in registers until the compacted primitive count is known; see MeshletVisBuffer.slang.
    FMeshVertexOutput VOut = (FMeshVertexOutput)0;
    float4            Clip = float4(0.0, 0.0, 0.0, 0.0);

    if (ThreadID < VertCount)
    {
        // Issued before the vertex fetch so the loads overlap rather than serialize; see the same
        // block in MeshletVisBuffer.slang. Kept inside the guard because an unresolved meshlet leaves
        // Instances() possibly null, making Ref.Inst a null base.
        const float4x4 ModelMatrix = Ref.Inst.ModelMatrix;

        FVertexData VertexData = LoadMeshletVertexData(Ref, ThreadID);
        FMeshletWorldVertex W  = TransformMeshletVertex(VertexData, ModelMatrix, CalculateNormalMatrix(ModelMatrix));

        // Same material vertex-graph local contract as MeshletVisBuffer.slang
        // (the codegen alias preamble reads Inst / WorldPos / NormalWS / VertexData by these names).
        FGPUInstance* Inst   = Ref.Inst;   // pointer: FMeshletRef reaches the instance, see MeshletGeometry.slang
        float4   WorldPos    = float4(W.WorldPos, 1.0);
        float3   NormalWS    = W.NormalWS;
        float3   TangentWS   = W.TangentWS.xyz;
        float    TangentSignWS = W.TangentWS.w;   // handedness; the graph's WorldTangent alias needs it

        FMaterialVertexInputs Material;
        $MATERIAL_VERTEX_INPUTS
        WorldPos.xyz += Material.WorldPositionOffset;

        const float3 FinalWorldPos = WorldPos.xyz;

#ifdef MESHLET_MESH_BASE
        VOut = MakeBaseVertexOutput(Ref, W, FinalWorldPos, GetCameraView(), GetCameraProjection());
        Clip = VOut.Position;   // taken from the output so the two cannot drift
#else
        Clip = mul(GetShadowViewProjection(MeshletPass().ShadowDataIndex, (uint)MeshletPass().ViewIndex),
                   float4(FinalWorldPos, 1.0));
#endif
    }

    gsClipPos[ThreadID] = Clip;
    GroupMemoryBarrierWithGroupSync();

    // Triangle cull. The clip positions above are in THIS pass's space (the shadow VP for a shadow view,
    // the camera VP for depth), so the frustum reject is against the volume actually being rastered and
    // the winding is the one the hardware will see.
    const float2 ViewportSize = float2(MeshletPass().ViewportW, MeshletPass().ViewportH);

    uint3 TriIdx[MESHLET_TRI_ITERS];

    [unroll]
    for (uint i = 0; i < MESHLET_TRI_ITERS; ++i)
    {
        TriIdx[i] = uint3(0u, 0u, 0u);

        const uint t = ThreadID + i * 64u;
        if (t >= TriCount)
        {
            continue;
        }

        const uint3 Idx = UnpackMeshletTriangleSafe(Ref, t);
        TriIdx[i] = Idx;

        if (IsDegenerateTriangleIndices(Idx))
        {
            continue;
        }

        if (IsMeshletTriangleCulled(gsClipPos[Idx.x], gsClipPos[Idx.y], gsClipPos[Idx.z], ViewportSize))
        {
            continue;
        }

        InterlockedOr(gsTriBits[t >> 5u], 1u << (t & 31u));
    }

    GroupMemoryBarrierWithGroupSync();

    const FMeshletTriCompaction Keep = MakeTriCompaction(gsTriBits[0], gsTriBits[1], gsTriBits[2], gsTriBits[3]);

    SetMeshOutputCounts(VertCount, Keep.Total);

    if (ThreadID < VertCount)
    {
        VOut.Position   = Clip;
        Verts[ThreadID] = VOut;
    }

    [unroll]
    for (uint i = 0; i < MESHLET_TRI_ITERS; ++i)
    {
        const uint t = ThreadID + i * 64u;
        if (t >= TriCount || !Keep.IsKept(t))
        {
            continue;
        }

        Tris[Keep.OutputSlot(t)] = TriIdx[i];
    }
}
