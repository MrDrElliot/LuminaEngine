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
    uint VisID : SV_PrimitiveID;
};

// The per-vertex output type this variant declares. Masked needs the interpolant set; opaque needs a clip
// position and nothing else (VisID is per-PRIMITIVE, below).
#ifdef VISBUFFER_MASKED_GEOM
typedef FBaseVertexOutput      FVisVertexOutput;
#else
typedef FDepthOnlyVertexOutput FVisVertexOutput;
#endif

// Clip positions of THIS workgroup's meshlet, published by the vertex phase for the triangle phase. At 64
// threads and MESHLET_MAX_VERTICES == 64 lane i owns vertex i, so every other per-vertex result stays in
// that lane's registers across the barrier and only this -- which the triangle phase reads for vertices it
// does not own -- has to go through groupshared. 1KB, well under what caps mesh-workgroup residency.
groupshared float4 gsClipPos[MESHLET_MAX_VERTICES];
// One bit per triangle that survived the cull.
groupshared uint   gsTriBits[MESHLET_TRI_WORDS];

// Sized to the VERTEX phase (MESHLET_MAX_VERTICES = 64), not the triangle cap (124): the vertex phase is the
// expensive one (BDA load + normal matrix + WPO graph + 2 mat-vec muls), so all 64 lanes must do vertex work.
// The cheap triangle phase grid-strides over the 64 lanes (<=124 tris -> 2 iterations). At 128 threads the
// vertex phase ran at ~50% lane utilization (lanes 64..127 idle through the costly work).
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(64, 1, 1)]
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

    // Zeroed here rather than in its own guarded block so the single barrier below orders BOTH this and
    // the clip-position writes; the triangle phase is the first thing that reads either.
    if (ThreadID < MESHLET_TRI_WORDS)
    {
        gsTriBits[ThreadID] = 0u;
    }

    // ------------------------------------------------------------------------------------------------
    // Vertex phase. The output is NOT written yet: SetMeshOutputCounts has to publish the compacted
    // primitive count first, and that is not known until the triangle phase has run. The result is held
    // in this lane's registers instead -- lane i owns vertex i, so nothing is shared but the clip position.
    // ------------------------------------------------------------------------------------------------
    FVisVertexOutput VOut = (FVisVertexOutput)0;
    float4           Clip = float4(0.0, 0.0, 0.0, 0.0);

    if (ThreadID < VertCount)
    {
        // Every workgroup-uniform matrix load is issued HERE, before the vertex fetch, so all four
        // requests are in flight together. They used to be strung end to end -- vertex data, then the
        // model matrix inside TransformMeshletVertex (twice), then the camera view, then the camera
        // projection -- each starting only once the previous result was consumed. Nsight put more
        // dependent stall samples on GetCameraView/GetCameraProjection than on anything else in this
        // pass, and the whole pass sat at 93% long-scoreboard.
        //
        // Deliberately INSIDE the vertex guard. A meshlet that failed to resolve leaves VertCount at 0
        // and Instances() may be null, so Ref.Inst is null+0 -- hoisting the model-matrix load above
        // this branch would dereference it on exactly the path the guard exists to protect.
        const float4x4 ModelMatrix = Ref.Inst.ModelMatrix;
        const float4x4 CameraView  = GetCameraView();
        const float4x4 CameraProj  = GetCameraProjection();

        FVertexData VertexData = LoadMeshletVertexData(Ref, ThreadID);
        FMeshletWorldVertex W  = TransformMeshletVertex(VertexData, ModelMatrix, CalculateNormalMatrix(ModelMatrix));

        // Same material vertex-graph local contract as MeshletMesh/MeshletVertex (WPO must match so the
        // VisBuffer depth agrees with what the deferred pass reconstructs). ModelMatrix above is the
        // graph's alias too -- it is declared here for that reason as much as for the transform.
        FGPUInstance* Inst   = Ref.Inst;   // pointer: FMeshletRef reaches the instance, see MeshletGeometry.slang
        float4   WorldPos    = float4(W.WorldPos, 1.0);
        float3   NormalWS    = W.NormalWS;
        float3   TangentWS   = W.TangentWS.xyz;
        float    TangentSignWS = W.TangentWS.w;   // handedness; the graph's WorldTangent alias needs it

        FMaterialVertexInputs Material;
        $MATERIAL_VERTEX_INPUTS
        WorldPos.xyz += Material.WorldPositionOffset;

#ifdef VISBUFFER_MASKED_GEOM
        VOut = MakeBaseVertexOutput(Ref, W, WorldPos.xyz, CameraView, CameraProj);
        Clip = VOut.Position;   // the same two muls; taken from the output so the two cannot drift
#else
        Clip          = CameraClipPosition(CameraViewPosition(WorldPos.xyz, CameraView), CameraProj);
        VOut.Position = Clip;
#endif
    }

    // Lanes past VertCount publish a zero position. Nothing reads it: UnpackMeshletTriangleSafe bounds
    // every index against VertCount, so a triangle can only ever name a vertex a lane actually wrote.
    gsClipPos[ThreadID] = Clip;
    GroupMemoryBarrierWithGroupSync();

    // ------------------------------------------------------------------------------------------------
    // Triangle phase: cull, then compact. Indices are decoded ONCE and kept in registers -- the loop below
    // runs twice at the 124-triangle cap, so re-decoding for the emit would double the triangle-array
    // traffic for nothing.
    // ------------------------------------------------------------------------------------------------
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

        if (IsMeshletTriangleCulled(gsClipPos[Idx.x], gsClipPos[Idx.y], gsClipPos[Idx.z],
                                    float2(GetScreenSize())))
        {
            continue;
        }

        InterlockedOr(gsTriBits[t >> 5u], 1u << (t & 31u));
    }

    GroupMemoryBarrierWithGroupSync();

    const FMeshletTriCompaction Keep = MakeTriCompaction(gsTriBits[0], gsTriBits[1], gsTriBits[2], gsTriBits[3]);

    // Uniform across the workgroup: every lane read the same four words after the same barrier.
    SetMeshOutputCounts(VertCount, Keep.Total);

    if (ThreadID < VertCount)
    {
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

        // VisID packs the SOURCE triangle index, never the compacted slot: the deferred pass re-fetches
        // the triangle out of the meshlet's own array, which this compaction does not touch.
        const uint Slot   = Keep.OutputSlot(t);
        Tris[Slot]        = TriIdx[i];
        Prims[Slot].VisID = ((MeshletSlot << 7u) | (t & 0x7Fu)) + 1u;
    }
}
