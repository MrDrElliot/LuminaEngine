#pragma once
#include "Includes/MeshletGeometry.slang"

// VisBuffer geometry MESH shader: rasterize meshlets writing a per-primitive visibility ID + depth. One
// workgroup per meshlet, like MeshletMesh.
//
// Compiled TWICE, selected by the VISBUFFER_MASKED_GEOM macro, because the two cases need different OUTPUT
// TYPES and a spec constant cannot change one:
//   - undefined (opaque): per-vertex output is position only. The opaque mesh-path pixel shader reads
//     SV_PrimitiveID and nothing else, so there is no interpolant for it to consume.
//   - defined (masked): full per-vertex base interpolants, so the masked pixel shader can run the opacity
//     graph and discard cut-out texels before they write VisID/depth. Compiled for masked materials only.
//
// This used to be one module branching on a spec constant, writing a zeroed FBaseVertexOutput on the opaque
// path and claiming the interpolants "dead-strip per-PSO". They do not: mesh output storage is allocated
// from the DECLARED type at pipeline creation, so every opaque meshlet reserved 124B per vertex (7.9KB per
// workgroup) to carry 16B of SV_Position, and the geometry stage still fed TRAM for attributes nothing read.
// Measured at 64.2% of warp-launch cycles stalled on PS TRAM fill. See FDepthOnlyVertexOutput.
//
// WPO always applies (the rasterized geometry must match what the deferred pass reconstructs).
//
// VisID is per-PRIMITIVE via the SV_PrimitiveID builtin (a user-defined per-primitive fragment input can't be
// decorated PerPrimitiveEXT in slang #7019; AMD rejects it). VisID packing: [31:7] global MeshletDrawList slot,
// [6:0] local triangle index; stored as VisID + 1 so clear-to-0 marks empty.

// One CmdDrawMeshTasksIndirect covers a whole batch; SV_DrawIndex selects the sub-draw. The per-draw meshlet
// base comes from ViewDrawOffsets, the same GPU-built array the task stage appended through (so the
// mesh and VS paths consume the same value). It used to be hand-indexed out of FDrawIndirectArguments as raw
// uints ([arg*4 + 3]), which meant the mesh-task path reached into a struct belonging to the graphics
// indirect path and read a cacheline other lanes were atomically incrementing (InstanceCount is element 1 of
// the same 16 bytes).
//
// The meshlet always arrives through the task stage's payload -- there is exactly one geometry path and it
// is task + mesh. FMeshletPassArgs (MeshletGeometry.slang) is the push block both stages share.

struct FVisPrimitive
{
    uint VisID          : SV_PrimitiveID;
    // Hardware-side reject. Declaring every triangle and flagging the dead ones is cheaper than compacting
    // them: compaction needed a shared bitmask, an atomic per surviving lane onto one of two words, and a
    // second barrier. Output storage is reserved from the DECLARED array size either way, so compacting
    // never bought allocation, only cost.
    bool CullPrimitive  : SV_CullPrimitive;
};

// The per-vertex output type this variant declares. Masked needs the interpolant set; opaque needs a clip
// position and nothing else (VisID is per-PRIMITIVE, below).
#ifdef VISBUFFER_MASKED_GEOM
typedef FBaseVertexOutput      FVisVertexOutput;
#else
typedef FDepthOnlyVertexOutput FVisVertexOutput;
#endif


// Sized to the VERTEX phase (MESHLET_MAX_VERTICES = 64), not the triangle cap (124): the vertex phase is the
// expensive one (BDA load + normal matrix + WPO graph + 2 mat-vec muls), so all 64 lanes must do vertex work.
// The cheap triangle phase grid-strides over the 64 lanes (<=124 tris -> 2 iterations). At 128 threads the
// vertex phase ran at ~50% lane utilization (lanes 64..127 idle through the costly work).
// The material vertex graph, emitted ONCE. The token substitutes raw statements that reach these by name,
// so they arrive as parameters: left inside the vertex loop, the whole graph body would be emitted per
// iteration. WPO must match MeshletMesh's so the VisBuffer depth agrees with the deferred reconstruction.
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
    in payload     FVisTaskPayload  TaskPayload,
    out vertices   FVisVertexOutput Verts[MESHLET_MAX_VERTICES],
    out indices    uint3            Tris[MESHLET_MAX_TRIANGLES],
    out primitives FVisPrimitive    Prims[MESHLET_MAX_TRIANGLES])
{
    // GroupID.x indexes the task stage's COMPACTED survivor list, not the draw-list region -- the task
    // shader dropped the entries that failed to resolve, so there is no correspondence between this
    // workgroup's id and a slot offset. The slot it does own is carried in the entry.
    const uint  MeshletSlot = TaskPayload.Entries[GroupID.x].MeshletSlot;
    FMeshletRef Ref         = MakeMeshletRefFromTask(TaskPayload, GroupID.x);

    const uint VertCount = Ref.M.VertexCount;
    const uint TriCount  = Ref.M.TriangleCount;

    // ------------------------------------------------------------------------------------------------
    // Vertex phase. The output is NOT written yet: SetMeshOutputCounts has to publish the compacted
    // primitive count first, and that is not known until the triangle phase has run. The result is held
    // in this lane's registers instead -- lane i owns vertex i, so nothing is shared but the clip position.
    // ------------------------------------------------------------------------------------------------
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
        // Every workgroup-uniform matrix load is issued HERE, before the vertex fetch, so all four
        // requests are in flight together. They used to be strung end to end -- vertex data, then the
        // model matrix inside TransformMeshletVertex (twice), then the camera view, then the camera
        // projection -- each starting only once the previous result was consumed. Nsight put more
        // dependent stall samples on GetCameraView/GetCameraProjection than on anything else in this
        // pass, and the whole pass sat at 93% long-scoreboard.
        const float4x4 ModelMatrix  = Ref.Inst.ModelMatrix;
        const float3x3 NormalMatrix = CalculateNormalMatrix(ModelMatrix);
        const float4x4 CameraView   = GetCameraView();
        const float4x4 CameraProj   = GetCameraProjection();

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
