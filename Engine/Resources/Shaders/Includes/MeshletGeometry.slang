#pragma once
#include "SceneGlobals.slang"

// Shared meshlet geometry path for BOTH the vertex-shader emulation (non-indexed, corner-decoded draws)
// and the mesh-shader path. Anything the two must agree on lives here so they can never drift:
//   - resolving a draw-list slot to its instance + meshlet,
//   - loading a vertex (skinned/static),
//   - unpacking a meshlet triangle,
//   - the model->world transform (position / normal / tangent),
//   - the base-pass interpolant layout (FBaseVertexOutput) + how it's filled,
//   - the camera clip transform (base pass and depth pre-pass MUST match for early-Z).
//
// What deliberately stays in the individual shaders (it genuinely differs):
//   - VS-emulation corner decode + degenerate cull   vs   mesh workgroup vertex/primitive loop,
//   - the material $MATERIAL_VERTEX_INPUTS token (text-substituted inline by the material compiler),
//   - the pass-specific final position (camera VP vs shadow VP) and which outputs are written.

// ---------------------------------------------------------------------------------------------------
// Resolve: draw-list slot -> instance + meshlet
// ---------------------------------------------------------------------------------------------------

struct FMeshletRef
{
    FGPUInstance    Inst;
    FMeshlet        M;
    uint            InstanceID;
    uint            GlobalMeshletIdx;
};

// bShadow selects the instance's shadow meshlet range instead of the surface (base/depth) range.
FMeshletRef ResolveMeshlet(uint MeshletSlot, bool bShadow)
{
    FMeshletDraw Draw = MeshletDrawList()[MeshletSlot];

    FMeshletRef Ref;
    Ref.Inst       = GetInstance(Draw.InstanceID);
    Ref.InstanceID = Draw.InstanceID;

    uint BaseOffset      = bShadow ? Ref.Inst.ShadowMeshletOffset : Ref.Inst.SurfaceMeshletOffset;
    Ref.GlobalMeshletIdx = BaseOffset + Draw.MeshletLocalIndex;
    Ref.M                = Ref.Inst.MeshletHeader.Meshlets[Ref.GlobalMeshletIdx];
    return Ref;
}

FMeshletRef ResolveSurfaceMeshlet(uint MeshletSlot) { return ResolveMeshlet(MeshletSlot, false); }
FMeshletRef ResolveShadowMeshlet(uint MeshletSlot)  { return ResolveMeshlet(MeshletSlot, true);  }

// ---------------------------------------------------------------------------------------------------
// Vertex + triangle access
// ---------------------------------------------------------------------------------------------------

// SPEC_SKINNED dead-strips the unused vertex-load path for homogeneous batches: 0=static-only, 1=skinned-only,
// 2=dynamic (per-instance runtime branch, used by mixed batches and by passes that don't specialize, e.g. deferred).
[vk::constant_id(5)] const uint SPEC_SKINNED = 2;

FVertexData LoadMeshletVertexData(FMeshletRef Ref, uint LocalVertIdx)
{
    bool bSkinned;
    if      (SPEC_SKINNED == 0u) { bSkinned = false; }
    else if (SPEC_SKINNED == 1u) { bSkinned = true;  }
    else                         { bSkinned = Ref.Inst.HasFlag(EInstanceFlags::Skinned); }

    if (bSkinned)
    {
        return LoadSkinnedVertex(Ref.M, LocalVertIdx, Ref.Inst.SkinnedVertexBase);
    }
    return LoadStaticVertex(Ref.Inst.MeshletHeader, Ref.M, LocalVertIdx);
}

// One packed dword = three 8-bit local vertex indices (the meshlet's triangle).
uint3 UnpackMeshletTriangle(FMeshletRef Ref, uint TriIdx)
{
    uint Packed = Ref.Inst.MeshletHeader.Triangles[Ref.M.TriangleOffset + TriIdx];
    return uint3(Packed & 0xFFu, (Packed >> 8u) & 0xFFu, (Packed >> 16u) & 0xFFu);
}

// VS-emulation single-corner decode: corner in [0,3) of triangle TriIdx -> local vertex index.
uint MeshletCornerVertexIndex(FMeshletRef Ref, uint TriIdx, uint Corner)
{
    uint Packed = Ref.Inst.MeshletHeader.Triangles[Ref.M.TriangleOffset + TriIdx];
    return (Packed >> (Corner * 8u)) & 0xFFu;
}

// ---------------------------------------------------------------------------------------------------
// Model -> world transform (pre-WPO). Identical for every pass and both draw paths.
// ---------------------------------------------------------------------------------------------------

struct FMeshletWorldVertex
{
    float3 WorldPos;     // pre-WorldPositionOffset
    float3 NormalWS;
    float4 TangentWS;    // xyz: world tangent, w: MikkTSpace handedness
    float2 UV;
    float4 Color;
};

// Overload taking a precomputed world normal matrix: the deferred reconstruction transforms three
// vertices of one triangle, all sharing the instance's ModelMatrix, so the inverse-transpose is computed
// once by the caller instead of three times here.
FMeshletWorldVertex TransformMeshletVertex(FMeshletRef Ref, FVertexData V, float3x3 NormalMatrixWS)
{
    float4x4 ModelMatrix = Ref.Inst.ModelMatrix;

    FMeshletWorldVertex W;
    W.WorldPos  = mul(ModelMatrix, float4(V.Position, 1.0)).xyz;
    W.NormalWS  = mul(NormalMatrixWS, V.Normal);
    // Tangents take the forward 3x3 (they lie in the surface); re-orthogonalized per-pixel.
    W.TangentWS = float4(mul((float3x3)ModelMatrix, V.Tangent.xyz), V.Tangent.w);
    W.UV        = V.UV;
    W.Color     = V.Color;
    return W;
}

FMeshletWorldVertex TransformMeshletVertex(FMeshletRef Ref, FVertexData V)
{
    return TransformMeshletVertex(Ref, V, CalculateNormalMatrix(Ref.Inst.ModelMatrix));
}

// Camera view/clip. Base pass and depth pre-pass MUST derive SV_Position from this exact pair so the
// [earlydepthstencil] equal-depth test holds bit-for-bit. Both go through CameraViewPosition first.
float4 CameraViewPosition(float3 WorldPos) { return mul(GetCameraView(), float4(WorldPos, 1.0)); }
float4 CameraClipPosition(float4 ViewPos)  { return mul(GetCameraProjection(), ViewPos); }

// ---------------------------------------------------------------------------------------------------
// Base-pass interpolants: the single source of truth, produced by the VS emulation AND the mesh shader,
// and consumed by BasePixelPass. Per-instance constants are forwarded flat so the PS never re-reads
// the 128B FGPUInstance per pixel.
// ---------------------------------------------------------------------------------------------------

struct FBaseVertexOutput
{
    float4 FragColor                   : COLOR;
    float4 NormalVS                    : NORMAL_VS;
    float4 NormalWS                    : NORMAL_WS;
    float4 TangentWS                   : TANGENT_WS;
    float4 FragPos                     : FRAG_POS;
    float2 UV                          : TEXCOORD0;
    uint   InstanceIndex               : INSTANCE;
    nointerpolation uint EntityID      : ENTITY_ID;
    nointerpolation uint MaterialIndex : MATERIAL_INDEX;
    nointerpolation uint InstanceFlags : INSTANCE_FLAGS;
    nointerpolation uint MeshletIndex  : MESHLET_INDEX;
    precise float4 Position            : SV_Position;
};

// VisBuffer vertex-emulation geometry output: the base interpolants + the flat VisID. The interpolants are
// only computed when the VISBUFFER_MASKED spec constant is set (so the masked pixel shader can run the opacity
// graph -- which may read UV / normal / world position); for opaque materials the spec constant is 0 and the
// interpolant compute + outputs dead-strip at pipeline specialization (same pattern as the EPass spec constant).
struct FVisVertexOut
{
    // VisID FIRST so it gets interface location 0 -- the opaque pixel shader reads only {VisID, SV_Position}
    // and must link to the same location; the Base interpolants (locations 1+) are then unconsumed by the
    // opaque PS (not interpolated) and consumed only by the masked PS. SPIR-V-verified.
    nointerpolation uint VisID : VIS_ID;
    FBaseVertexOutput   Base;
};

// FinalWorldPos is post-WPO (the shader adds Material.WorldPositionOffset before calling this).
FBaseVertexOutput MakeBaseVertexOutput(FMeshletRef Ref, FMeshletWorldVertex W, float3 FinalWorldPos)
{
    float4 ViewPos = CameraViewPosition(FinalWorldPos);

    FBaseVertexOutput O;
    O.FragColor     = W.Color;
    O.NormalVS      = float4(normalize(NormalWorldToView(W.NormalWS)), 1.0);
    O.NormalWS      = float4(W.NormalWS, 1.0);
    O.TangentWS     = W.TangentWS;
    O.FragPos       = ViewPos;
    O.UV            = W.UV;
    O.InstanceIndex = Ref.InstanceID;
    O.EntityID      = Ref.Inst.EntityID;
    O.MaterialIndex = Ref.Inst.MaterialIndex;
    O.InstanceFlags = (uint)Ref.Inst.Flags;
    O.MeshletIndex  = Ref.GlobalMeshletIdx;
    O.Position      = CameraClipPosition(ViewPos);
    return O;
}

// Off-screen degenerate vertex for VS-emulation invocations past the meshlet's triangle count.
FBaseVertexOutput MakeDegenerateBaseVertexOutput()
{
    const float NaN = 0.0f / 0.0f;

    FBaseVertexOutput O;
    O.FragColor     = float4(0, 0, 0, 0);
    O.NormalVS      = float4(0, 0, 0, 0);
    O.NormalWS      = float4(0, 0, 0, 0);
    O.TangentWS     = float4(0, 0, 0, 0);
    O.FragPos       = float4(0, 0, 0, 0);
    O.UV            = float2(0, 0);
    O.InstanceIndex = 0;
    O.EntityID      = 0;
    O.MaterialIndex = 0;
    O.InstanceFlags = 0;
    O.MeshletIndex  = 0;
    O.Position      = float4(NaN, NaN, NaN, 1.0);
    return O;
}
