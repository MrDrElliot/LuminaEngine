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
// base comes from ViewDrawOffsets, the same GPU-built array SeedIndirectArgs seeds FirstInstance from (so the
// mesh and VS paths consume the same value). It used to be hand-indexed out of FDrawIndirectArguments as raw
// uints ([arg*4 + 3]), which meant the mesh-task path reached into a struct belonging to the graphics
// indirect path and read a cacheline other lanes were atomically incrementing (InstanceCount is element 1 of
// the same 16 bytes).
struct FVisPassArgs
{
    uint* ViewDrawOffsets;   // per-(view, draw) base into MeshletDrawList()
    uint  ArgBase;           // first arg index for this batch: ViewIndex*NumDrawsPerView + IndirectDrawOffset
};
FVisPassArgs* Pass() { return GetPassArgs<FVisPassArgs>(); }

uint LoadMeshletBase(uint DrawIndex)
{
    return Pass().ViewDrawOffsets[Pass().ArgBase + DrawIndex];
}

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
    uint  DrawIndex : SV_DrawIndex,
    out vertices   FVisVertexOutput Verts[MESHLET_MAX_VERTICES],
    out indices    uint3            Tris[MESHLET_MAX_TRIANGLES],
    out primitives FVisPrimitive    Prims[MESHLET_MAX_TRIANGLES])
{
    uint MeshletSlot = LoadMeshletBase(DrawIndex) + GroupID.x;
    FMeshletRef Ref  = ResolveSurfaceMeshlet(MeshletSlot);

    const uint VertCount = Ref.M.VertexCount;
    const uint TriCount  = Ref.M.TriangleCount;
    SetMeshOutputCounts(VertCount, TriCount);

    if (ThreadID < VertCount)
    {
        FVertexData VertexData = LoadMeshletVertexData(Ref, ThreadID);
        FMeshletWorldVertex W  = TransformMeshletVertex(Ref, VertexData);

        // Same material vertex-graph local contract as MeshletMesh/MeshletVertex (WPO must match so the
        // VisBuffer depth agrees with what the deferred pass reconstructs).
        FGPUInstance Inst    = Ref.Inst;
        float4x4 ModelMatrix = Ref.Inst.ModelMatrix;
        float4   WorldPos    = float4(W.WorldPos, 1.0);
        float3   NormalWS    = W.NormalWS;
        float3   TangentWS   = W.TangentWS.xyz;
        float    TangentSignWS = W.TangentWS.w;   // handedness; the graph's WorldTangent alias needs it

        FMaterialVertexInputs Material;
        $MATERIAL_VERTEX_INPUTS
        WorldPos.xyz += Material.WorldPositionOffset;

#ifdef VISBUFFER_MASKED_GEOM
        Verts[ThreadID] = MakeBaseVertexOutput(Ref, W, WorldPos.xyz);
#else
        FVisVertexOutput O;
        O.Position = CameraClipPosition(CameraViewPosition(WorldPos.xyz));
        Verts[ThreadID] = O;
#endif
    }

    // Triangle phase grid-strides the 64 lanes (2 iterations at the 124-tri cap). VisID packs the LOOP var.
    for (uint t = ThreadID; t < TriCount; t += 64u)
    {
        Tris[t]        = UnpackMeshletTriangleSafe(Ref, t);
        Prims[t].VisID = ((MeshletSlot << 7u) | (t & 0x7Fu)) + 1u;
    }
}
