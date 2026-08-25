// Compute lane: no rasterizer, so nothing here may use ddx/ddy or an implicit-derivative sample.
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/Wind.slang"   // this template substitutes the material vertex token too (WPO reconstruction)

// Per-material GBuffer generation over exactly the pixels the classify passes proved that material
// owns. COMPUTE, not a quad: every thread is a real pixel, so derivatives must be analytic.

// Offsets 0-12 overlay FDBufferPushConstants (DBuffer.slang) so the shared BuildGBufferData -> ApplyDBuffer
// reads the decal indices out of this same block: offset 0 is read there as an (unused) MeshletBase.
struct FDeferredMaterialArgs
{
    uint  VisBufferIndex;   // 0
    uint  DBufferAIndex;    // 4
    uint  DBufferBIndex;    // 8
    uint  DBufferCIndex;    // 12
    uint  DrawListCount;    // 16  MeshletDrawList slot bound
    uint  SlotIndex;        // 20  this dispatch's dense material slot
    uint  ScreenW;          // 24
    uint  ScreenH;          // 28
    uint  GBufferAUAV;      // 32  storage-heap slots, channel order per Includes/GBuffer.slang
    uint  GBufferBUAV;      // 36
    uint  GBufferCUAV;      // 40
    uint  GBufferDUAV;      // 44
    uint* PixelList;        // 48  packed screen positions, one contiguous run per slot
    uint* Starts;           // 56  uint[MATERIAL_MAX_SLOTS]
    uint* Counts;           // 64  uint[MATERIAL_MAX_SLOTS]
    uint  VelocityUAV;      // 72  0xFFFFFFFF when temporal AA is off
    uint  _PadVelocity;     // 76
};
FDeferredMaterialArgs* Pass() { return GetPassArgs<FDeferredMaterialArgs>(); }

// Object space at the previous frame's pose, so the caller only has to apply the previous model matrix.
float3 PrevObjectPosition(FMeshletRef Ref, uint LocalVertIdx, FVertexData Current)
{
    bool bSkinned;
    if      (SPEC_SKINNED == 0u) { bSkinned = false; }
    else if (SPEC_SKINNED == 1u) { bSkinned = true;  }
    else                         { bSkinned = Ref.Inst.HasFlag(EInstanceFlags::Skinned); }

    // A shorter snapshot cannot be indexed safely, because ResolvePaletteBone clamps against the live count.
    const bool bPrevPoseUsable = (PrevBones() != nullptr)
                              && (GetSceneRoot().PrevBoneCount >= Scene().CullData.BoneNum);

    if (bSkinned && bPrevPoseUsable)
    {
        const FPreSkinnedVertex Prev = SkinVertexWith(PrevBones(), Ref.Inst.MeshletHeader, Ref.M,
                                                      Ref.GlobalMeshletIdx, LocalVertIdx, Ref.Inst.BoneOffset);
        return float3(Prev.Px, Prev.Py, Prev.Pz);
    }

    return Current.Position;
}

float4x4 PrevModelMatrixOf(FGPUInstance* Inst, float4x4 Fallback)
{
    FTransform3x4* Prev = PrevRetainedTransforms();
    const uint Slot = Inst.RetainedSlot;
    if (Prev == nullptr || Slot >= GetSceneRoot().PrevRetainedTransformCount)
    {
        return Fallback;
    }
    return Prev[Slot].ToMatrix();
}

[shader("compute")]
[numthreads(MATERIAL_PIXEL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    const uint Slot = Pass().SlotIndex;

    // The grid is folded across Y because a 4K frame's pixel count overruns maxComputeWorkGroupCount[0];
    // VisBufferMaterialPrefixSum built it with the matching fold.
    const uint LinearGroup = GroupID.y * MAX_DISPATCH_AXIS + GroupID.x;
    const uint Index       = LinearGroup * MATERIAL_PIXEL_GROUP_SIZE + Lane;
    if (Index >= Pass().Counts[Slot])
    {
        return;
    }

    const uint2 Texel = UnpackVisPixel(Pass().PixelList[Pass().Starts[Slot] + Index]);

    const uint   Raw = LoadTexture2DUInt(Pass().VisBufferIndex, int2(Texel));
    FVisTriangle T   = ResolveVisTriangle(Raw, Pass().DrawListCount);

    FGBufferData G = DefaultGBufferData();
    if (T.bValid)
    {
        // The three corners share the instance's ModelMatrix, so load it ONCE. Reaching it through Ref.Inst
        // per corner is a fresh dependent PhysicalStorageBuffer fetch that SPIR-V LICM will not merge.
        const float4x4 ModelMatrix    = T.Ref.Inst.ModelMatrix;
        float3x3       NormalMatrixWS = CalculateNormalMatrix(ModelMatrix);

        // Re-apply WorldPositionOffset per corner so the rebuilt triangle matches what the geometry pass
        // rasterized. Load and transform are FUSED so only one corner's FVertexData (48 VGPRs) is live.
        FGPUInstance* Inst = T.Ref.Inst;   // reached, not stored -- see FMeshletRef
        const bool bWantVelocity = Pass().VelocityUAV != 0xFFFFFFFFu;
        const float4x4 PrevModel = bWantVelocity ? PrevModelMatrixOf(T.Ref.Inst, ModelMatrix) : ModelMatrix;

        FMeshletWorldVertex W[3];
        float3 FinalWorldPos[3];
        float3 PrevWorldPos[3];
        [unroll]
        for (int k = 0; k < 3; ++k)
        {
            FVertexData VertexData = LoadMeshletVertexData(T.Ref, T.Tri[k]);
            W[k] = TransformMeshletVertex(VertexData, ModelMatrix, NormalMatrixWS);

            // World position offset is deliberately not re-evaluated at the previous time; wind does not move here.
            PrevWorldPos[k] = bWantVelocity
                            ? mul(PrevModel, float4(PrevObjectPosition(T.Ref, T.Tri[k], VertexData), 1.0)).xyz
                            : float3(0.0);

            float4 WorldPos      = float4(W[k].WorldPos, 1.0);
            float3 NormalWS      = W[k].NormalWS;
            float3 TangentWS     = W[k].TangentWS.xyz;
            float  TangentSignWS = W[k].TangentWS.w;   // handedness; the graph's WorldTangent alias needs it

            FMaterialVertexInputs Material;
            $MATERIAL_VERTEX_INPUTS
            FinalWorldPos[k] = W[k].WorldPos + Material.WorldPositionOffset;

            // A constant offset folds away here, so only a graph with real WPO pays for this second pass.
            if (bWantVelocity)
            {
                gMaterialVertexTime = GetPrevTime();

                WorldPos      = float4(PrevWorldPos[k], 1.0);
                NormalWS      = W[k].NormalWS;
                TangentWS     = W[k].TangentWS.xyz;
                TangentSignWS = W[k].TangentWS.w;

                FMaterialVertexInputs Material;
                $MATERIAL_VERTEX_INPUTS
                PrevWorldPos[k] += Material.WorldPositionOffset;

                gMaterialVertexTime = -1.0;
            }
        }

        const float2 ScreenSize = float2(Pass().ScreenW, Pass().ScreenH);
        FVisSurface  S          = InterpolateVisSurface(W, FinalWorldPos, VisPixelNdc(Texel, ScreenSize), ScreenSize);

        if (bWantVelocity)
        {
            const float3 PrevWorld = InterpBary(S.Bary, PrevWorldPos[0], PrevWorldPos[1], PrevWorldPos[2]);
            const float4 PrevClip  = mul(GetPrevViewProjection(), float4(PrevWorld, 1.0));

            float2 Motion = float2(0.0, 0.0);
            if (PrevClip.w > 0.0)
            {
                const float2 PrevUV = (PrevClip.xy / PrevClip.w) * 0.5 + 0.5;
                Motion = PrevUV - ((float2(Texel) + 0.5) / ScreenSize);
            }

            gRWTextures2D[Pass().VelocityUAV][int2(Texel)] = float4(Motion, 0.0, 0.0);
        }

        const float3 OrientedNormalWS = OrientVisNormal(T.Ref, S.NormalWS, S.WorldPosition);
        const float4 ViewPos          = CameraViewPosition(S.WorldPosition);
        const float4 ClipPos          = CameraClipPosition(ViewPos);
        const float  InvW             = 1.0 / max(ClipPos.w, 1e-6);

        // The interpolant struct the shared material/shading code consumes, rebuilt from the reconstruction.
        FBaseVertexOutput Input;
        Input.FragColor     = S.VertexColor;
        Input.NormalVS      = float4(normalize(NormalWorldToView(OrientedNormalWS)), 1.0);
        Input.NormalWS      = float4(OrientedNormalWS, 1.0);
        Input.TangentWS     = S.TangentWS;
        Input.FragPos       = ViewPos;
        Input.UV            = S.UV;
        Input.InstanceIndex = T.Ref.InstanceID;
        Input.EntityID      = T.Ref.Inst.EntityID;
        Input.MaterialIndex = T.Ref.Inst.MaterialIndex;
        Input.InstanceFlags = (uint)T.Ref.Inst.Flags;
        Input.MeshletIndex  = T.Ref.GlobalMeshletIdx;
        // What the rasterizer would have handed a pixel shader here: pixel center, NDC z, 1/w. Material
        // nodes read .xy for screen position/UV and .z for pixel depth.
        Input.Position      = float4(float2(Texel) + 0.5, ClipPos.z * InvW, InvW);

        // Material pixel graph locals (the names the pixel graph codegen reads).
        uint   EntityID       = Input.EntityID;
        uint   MaterialIndex  = Input.MaterialIndex;
        bool   bReceiveShadow = (Input.InstanceFlags & (uint)EInstanceFlags::ReceiveShadow) != 0u;
        float3 WorldPosition  = S.WorldPosition;
        float3 WorldNormal    = normalize(OrientedNormalWS);
        float4 WorldTangent   = Input.TangentWS;
        float2 UV0            = S.UV;
        float2 UV1            = S.UV1;
        float4 VertexColor    = S.VertexColor;
        float3 ViewPosition   = ViewPos.xyz;
        float2 UV0_DDX        = S.UVDDX;
        float2 UV0_DDY        = S.UVDDY;
        // Set 1 carries its own barycentric gradients for the same reason set 0 does: there is no 2x2
        // quad in this lane, so an implicit derivative does not exist to fall back on.
        float2 UV1_DDX        = S.UV1DDX;
        float2 UV1_DDY        = S.UV1DDY;

        // Analytic derivatives of the other varyings a graph can build a UV from; without these they sample
        // with UV0's gradient. WorldNormal deliberately has none -- it is normalized and two-sided-flipped.
        float3 WorldPosition_DDX = InterpWeights(S.Bary.DDX, FinalWorldPos[0], FinalWorldPos[1], FinalWorldPos[2]);
        float3 WorldPosition_DDY = InterpWeights(S.Bary.DDY, FinalWorldPos[0], FinalWorldPos[1], FinalWorldPos[2]);
        float4 VertexColor_DDX   = InterpWeights4(S.Bary.DDX, W[0].Color, W[1].Color, W[2].Color);
        float4 VertexColor_DDY   = InterpWeights4(S.Bary.DDY, W[0].Color, W[1].Color, W[2].Color);

        // Not named WorldNormal_DDX on purpose: this is the INTERPOLATED normal's gradient, kept out of
        // the material graph's alias set, and used only as the specular-AA variance estimate below.
        const float3 GeoNormalDDX = InterpWeights(S.Bary.DDX, W[0].NormalWS, W[1].NormalWS, W[2].NormalWS);
        const float3 GeoNormalDDY = InterpWeights(S.Bary.DDY, W[0].NormalWS, W[1].NormalWS, W[2].NormalWS);

        // One report per pixel, keyed by material slot -- see RequestMaterialResolution. This lane is where
        // residency demand comes from, because it is where the camera's view of a material is shaded, and
        // its gradients are the analytic barycentric ones (there is no quad here to take ddx/ddy from).
        RequestMaterialResolution(MaterialIndex, UV0_DDX, UV0_DDY, UV1_DDX, UV1_DDY);
        $MATERIAL_INPUTS

        G = BuildGBufferData(Input, Material);

        // No quad in this lane, so the widening comes from surface curvature rather than the normal map.
        G.Roughness = ApplyGeometricSpecularAA(G.Roughness, GeoNormalDDX, GeoNormalDDY);
    }

    const FGBufferPacked P = PackGBuffer(G);
    gRWTextures2D[Pass().GBufferAUAV][int2(Texel)] = P.A;
    gRWTextures2D[Pass().GBufferBUAV][int2(Texel)] = P.B;
    gRWTextures2D[Pass().GBufferCUAV][int2(Texel)] = P.C;
    gRWTextures2D[Pass().GBufferDUAV][int2(Texel)] = P.D;
}
