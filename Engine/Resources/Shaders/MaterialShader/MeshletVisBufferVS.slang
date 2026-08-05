#pragma once
#include "Includes/MeshletGeometry.slang"

// VisBuffer geometry, vertex-shader EMULATION path -- the fallback for GPUs without mesh shaders. Writes the
// visibility ID + clip position; the deferred material pass reconstructs everything else from the ID.
//
// One geometry shader serves BOTH opaque and masked materials via the VISBUFFER_MASKED spec constant:
//   - 0 (opaque): only the clip position is emitted; the base interpolants are zeroed and dead-strip at
//     pipeline specialization (the opaque pixel shader reads VisID only). Same lean output as before.
//   - 1 (masked): the full base interpolants are emitted so the masked pixel shader can run the material's
//     opacity graph and discard cut-out texels before they write VisID/depth.
// This mirrors the EPass spec constant (MeshletVertex.slang): one module, the unused path stripped per-PSO.
//
// VisID packing (uint32): [31:7] = global MeshletDrawList slot, [6:0] = local triangle index (<= 124).
// Stored as VisID + 1 so the natural clear-to-0 marks empty pixels. FirstInstance carries the meshlet base.

[vk::constant_id(4)] const uint VISBUFFER_MASKED = 0;

[shader("vertex")]
FVisVertexOut VertexMain(uint VertexIndex: SV_VulkanVertexID, uint MeshletSlot: SV_VulkanInstanceID)
{
    FMeshletRef Ref = ResolveSurfaceMeshlet(MeshletSlot);

    uint TriIdx = VertexIndex / 3u;
    uint Corner = VertexIndex % 3u;

    FVisVertexOut O;
    O.VisID = ((MeshletSlot << 7u) | (TriIdx & 0x7Fu)) + 1u;

    if (TriIdx >= Ref.M.TriangleCount)
    {
        O.Base = MakeDegenerateBaseVertexOutput();   // NaN position -> clipped
        return O;
    }

    uint LocalVertIdx      = MeshletCornerVertexIndex(Ref, TriIdx, Corner);
    FVertexData VertexData = LoadMeshletVertexData(Ref, LocalVertIdx);
    FMeshletWorldVertex W  = TransformMeshletVertex(Ref, VertexData);

    // Same material vertex-graph local contract as MeshletVertex/MeshletMesh so WPO matches bit-for-bit
    // (the deferred pass re-applies the identical WPO when it reconstructs this triangle).
    FGPUInstance Inst    = Ref.Inst;
    float4x4 ModelMatrix = Ref.Inst.ModelMatrix;
    float4   WorldPos    = float4(W.WorldPos, 1.0);
    float3   NormalWS    = W.NormalWS;
    float3   TangentWS   = W.TangentWS.xyz;
    float    TangentSignWS = W.TangentWS.w;   // handedness; the graph's WorldTangent alias needs it

    FMaterialVertexInputs Material;
    $MATERIAL_VERTEX_INPUTS
    WorldPos.xyz += Material.WorldPositionOffset;

    if (VISBUFFER_MASKED != 0u)
    {
        O.Base = MakeBaseVertexOutput(Ref, W, WorldPos.xyz);
    }
    else
    {
        // Opaque: only the clip position is consumed; interpolants stay zeroed and dead-strip per-PSO.
        O.Base = MakeDegenerateBaseVertexOutput();
        O.Base.Position = CameraClipPosition(CameraViewPosition(WorldPos.xyz));
    }
    return O;
}
