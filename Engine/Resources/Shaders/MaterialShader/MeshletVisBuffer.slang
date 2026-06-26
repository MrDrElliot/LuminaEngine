#pragma once
#include "Includes/MeshletGeometry.slang"

// VisBuffer geometry MESH shader: rasterize meshlets writing a per-primitive visibility ID + depth. One
// workgroup per meshlet, like MeshletMesh.
//
// One module serves BOTH opaque and masked via the VISBUFFER_MASKED spec constant (mirrors MeshletVisBufferVS):
//   - 0 (opaque): emit only clip position per vertex; the base interpolants are zeroed and dead-strip per-PSO
//     (the opaque pixel shader reads only VisID). The lean original behavior.
//   - 1 (masked): emit the full per-vertex base interpolants so the masked pixel shader can run the opacity
//     graph and discard cut-out texels before they write VisID/depth.
// WPO always applies (the rasterized geometry must match what the deferred pass reconstructs).
//
// VisID is per-PRIMITIVE via the SV_PrimitiveID builtin (a user-defined per-primitive fragment input can't be
// decorated PerPrimitiveEXT in slang #7019; AMD rejects it). VisID packing: [31:7] global MeshletDrawList slot,
// [6:0] local triangle index; stored as VisID + 1 so clear-to-0 marks empty.

// One CmdDrawMeshTasksIndirect covers a whole batch; SV_DrawIndex selects the sub-draw. The per-draw meshlet
// base is the cull's FDrawIndirectArguments.FirstInstance (the same value the VS path consumes) -- read it from
// the raw uint array (4 uints/arg, FirstInstance = element 3) instead of a per-sub-draw push constant.
struct FVisPassArgs
{
    uint* IndirectArgs;   // FDrawIndirectArguments[] as raw uints; [arg*4 + 3] = arg's FirstInstance = MeshletBase
    uint  ArgBase;        // first arg index for this batch: ViewIndex*NumDrawsPerView + IndirectDrawOffset
};
FVisPassArgs* Pass() { return GetPassArgs<FVisPassArgs>(); }

uint LoadMeshletBase(uint DrawIndex)
{
    return Pass().IndirectArgs[(Pass().ArgBase + DrawIndex) * 4u + 3u];
}

struct FVisPrimitive
{
    uint VisID : SV_PrimitiveID;
};

[vk::constant_id(4)] const uint VISBUFFER_MASKED = 0;

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
    out vertices   FBaseVertexOutput Verts[MESHLET_MAX_VERTICES],
    out indices    uint3             Tris[MESHLET_MAX_TRIANGLES],
    out primitives FVisPrimitive     Prims[MESHLET_MAX_TRIANGLES])
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

        FMaterialVertexInputs Material;
        $MATERIAL_VERTEX_INPUTS
        WorldPos.xyz += Material.WorldPositionOffset;

        if (VISBUFFER_MASKED != 0u)
        {
            Verts[ThreadID] = MakeBaseVertexOutput(Ref, W, WorldPos.xyz);
        }
        else
        {
            // Opaque: only the clip position is consumed; interpolants stay zeroed and dead-strip per-PSO.
            FBaseVertexOutput O = MakeDegenerateBaseVertexOutput();
            O.Position = CameraClipPosition(CameraViewPosition(WorldPos.xyz));
            Verts[ThreadID] = O;
        }
    }

    // Triangle phase grid-strides the 64 lanes (2 iterations at the 124-tri cap). VisID packs the LOOP var.
    for (uint t = ThreadID; t < TriCount; t += 64u)
    {
        Tris[t]        = UnpackMeshletTriangle(Ref, t);
        Prims[t].VisID = ((MeshletSlot << 7u) | (t & 0x7Fu)) + 1u;
    }
}
