#pragma once
#include "Includes/MeshletGeometry.slang"

// Unified meshlet vertex-EMULATION shader for all three geometry passes, selected by the EPass
// specialization constant (set per-pipeline at creation). Replaces BaseVertexPass / DepthPrePass /
// ShadowMappingVert. The shared geometry lives in MeshletGeometry.slang; this file is only the
// per-corner emulation scaffolding, the material WPO token, and the per-pass output/position.
//
// EPass: 0 = Base   (full interpolants, camera VP)
//        1 = Depth  (position only,    camera VP)   -- early-Z match with Base is bit-exact (same helpers)
//        2 = Shadow (position only,    shadow VP)
// Base computes the interpolants; Depth/Shadow gate that off, so on a specialized pipeline the unused
// work is dead-stripped. (The output struct is uniform so one module serves all three.)

[vk::constant_id(0)]
const uint EPass = 0;

static const uint EPASS_BASE   = 0u;
static const uint EPASS_DEPTH  = 1u;
static const uint EPASS_SHADOW = 2u;

// Shadow per-view push block; only read on the EPass == Shadow branch.
struct FShadowPassArgs
{
    int ShadowDataIndex;
    int ViewIndex;  // CSM cascade, point face, or 0 for spot.
};

[shader("vertex")]
FBaseVertexOutput VertexMain(uint VertexIndex: SV_VulkanVertexID, uint MeshletSlot: SV_VulkanInstanceID)
{
    FMeshletRef Ref = ResolveMeshlet(MeshletSlot, EPass == EPASS_SHADOW);

    uint TriIdx = VertexIndex / 3u;
    uint Corner = VertexIndex % 3u;
    if (TriIdx >= Ref.M.TriangleCount)
    {
        return MakeDegenerateBaseVertexOutput();
    }

    uint LocalVertIdx      = MeshletCornerVertexIndex(Ref, TriIdx, Corner);
    FVertexData VertexData = LoadMeshletVertexData(Ref, LocalVertIdx);
    FMeshletWorldVertex W  = TransformMeshletVertex(Ref, VertexData);

    // Locals the material vertex graph references; names/types kept stable for the substitution token
    // (the codegen alias preamble reads Inst / WorldPos / NormalWS / VertexData by these exact names).
    FGPUInstance* Inst   = Ref.Inst;   // pointer: FMeshletRef reaches the instance, see MeshletGeometry.slang
    float4x4 ModelMatrix = Ref.Inst.ModelMatrix;
    float4   WorldPos    = float4(W.WorldPos, 1.0);
    float3   NormalWS    = W.NormalWS;
    float3   TangentWS   = W.TangentWS.xyz;
    float    TangentSignWS = W.TangentWS.w;   // handedness; the graph's WorldTangent alias needs it

    // Material vertex graph emission (e.g. WorldPositionOffset); zero-init default leaves geometry unchanged.
    FMaterialVertexInputs Material;
    $MATERIAL_VERTEX_INPUTS
    WorldPos.xyz += Material.WorldPositionOffset;

    const float3 FinalWorldPos = WorldPos.xyz;

    if (EPass == EPASS_BASE)
    {
        return MakeBaseVertexOutput(Ref, W, FinalWorldPos);
    }

    // Depth + Shadow: position only. Interpolants left zero (no fragment stage reads them); the unused
    // interpolant compute in MakeBaseVertexOutput is dead-stripped on these specialized pipelines.
    FBaseVertexOutput O = MakeDegenerateBaseVertexOutput();
    if (EPass == EPASS_SHADOW)
    {
        FShadowPassArgs* Args = GetPassArgs<FShadowPassArgs>();
        O.Position = mul(GetShadowViewProjection(Args.ShadowDataIndex, (uint)Args.ViewIndex), float4(FinalWorldPos, 1.0));
    }
    else // EPASS_DEPTH -- identical path to Base's SV_Position for the equal-depth test.
    {
        O.Position = CameraClipPosition(CameraViewPosition(FinalWorldPos));
    }
    return O;
}
