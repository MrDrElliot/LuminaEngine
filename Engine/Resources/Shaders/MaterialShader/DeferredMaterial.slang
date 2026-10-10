// The compute lane has no rasterizer, so nothing here may use ddx/ddy or an implicit-derivative sample.
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Wind.slang"   // this template substitutes the material vertex token too (WPO reconstruction)

// Must match FDeferredMaterialPC in SceneRenderer_Lighting.cpp.
struct FDeferredMaterialArgs
{
    FTexture2DUInt VisBuffer;   // 0
    uint           FeedbackPhase;    // 4   which 1/STREAMING_FEEDBACK_WINDOW of the groups report streaming feedback this frame
    uint           NumSlots;         // 8   where the sparse half of Counts and Offsets starts
    FTexture2D     SlotImage;   // 12  slot + 1 per pixel from classify
    uint           DrawListCount;    // 16  MeshletDrawList slot bound
    uint           SlotIndex;        // 20
    uint           ScreenW;          // 24
    uint           ScreenH;          // 28
    FRWTexture2D   GBufferA;      // 32  storage-heap slots, channel order per Includes/GBuffer.slang
    FRWTexture2D   GBufferB;      // 36
    FRWTexture2D   GBufferC;      // 40
    FRWTexture2D   GBufferD;      // 44
    FRWTexture2D   Velocity;      // 48  0xFFFFFFFF when temporal AA is off
    uint           _PadVelocity;     // 52
    GPUSpan<uint>  PairList;   // 56  packed tile coordinates grouped by slot, each run starting at Offsets[slot]
    GPUSpan<uint>  Counts;     // 72  dense tiles per slot, then sparse pixels per slot
    GPUSpan<uint>  Offsets;    // 88  run starts in PairList, then in PixelList
    GPUSpan<uint>  PixelList;  // 104 packed pixel coordinates of the pairs too thin to shade as a tile
};
FDeferredMaterialArgs* Pass() { return GetPassArgs<FDeferredMaterialArgs>(); }

// Off without a velocity target, so the motion-vector path and its second vertex-graph run drop out of the register budget.
[vk::constant_id(9)] const uint SPEC_DEFERRED_VELOCITY = 1;

// Object space at the previous frame's pose, so the caller only has to apply the previous model matrix.
float3 PrevObjectPosition(FMeshletRef Ref, uint LocalVertIdx, FVertexData Current)
{
    bool bSkinned;
    if      (SPEC_SKINNED == 0u) { bSkinned = false; }
    else if (SPEC_SKINNED == 1u) { bSkinned = true;  }
    else                         { bSkinned = Ref.InstHasFlag(EInstanceFlags::Skinned); }

    // A shorter snapshot cannot be indexed safely, because ResolvePaletteBone clamps against the live count.
    const bool bPrevPoseUsable = (PrevBones().Count != 0u)
                              && (PrevBones().Count >= Scene().CullData.BoneNum);

    // The skinning pass already blended last frame's pose once per vertex, so a pre-skinned instance only reads it.
    const uint PrevIndex = Ref.SkinnedVertexBase + Ref.M.VertexOffset + LocalVertIdx;
    if (bSkinned && Ref.SkinnedVertexBase != kNoPreSkinBase && PreSkinnedPrevPositions().Contains(PrevIndex))
    {
        const FPrevSkinnedPosition Prev = PreSkinnedPrevPositions().Get(PrevIndex);
        return float3(Prev.X, Prev.Y, Prev.Z);
    }

    if (bSkinned && bPrevPoseUsable)
    {
        const FPreSkinnedVertex Prev = SkinVertexWith(PrevBones(), Ref.Header, Ref.M,
                                                      Ref.GlobalMeshletIdx, LocalVertIdx, Ref.Inst.BoneOffset);
        return float3(Prev.Px, Prev.Py, Prev.Pz);
    }

    return Current.Position;
}

float4x4 PrevModelMatrixOf(FGPUInstance* Inst, float4x4 Fallback)
{
    const uint Slot = Inst.RetainedSlot;
    if (!PrevRetainedTransforms().Contains(Slot))
    {
        return Fallback;
    }
    return PrevRetainedTransforms().Get(Slot).ToMatrix();
}

[shader("compute")]
[numthreads(MATERIAL_PIXEL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    const uint Slot = Pass().SlotIndex;

    // Folded across Y with the matching unfold in VisBufferMaterialArgs, the slot's dense tiles then its listed pixels 64 to a group.
    const uint LinearGroup = GroupID.y * MAX_DISPATCH_AXIS + GroupID.x;
    const uint DenseTiles  = Pass().Counts.Get(Slot);

    uint2 Texel;
    if (LinearGroup >= DenseTiles)
    {
        const uint SparseRun = Pass().NumSlots + Slot;
        const uint Entry     = (LinearGroup - DenseTiles) * MATERIAL_PIXEL_GROUP_SIZE + Lane;
        if (Entry >= Pass().Counts.Get(SparseRun))
        {
            return;
        }
        Texel = UnpackVisPixel(Pass().PixelList.Get(Pass().Offsets.Get(SparseRun) + Entry));
    }
    else
    {
        const uint2 Tile = UnpackVisPixel(Pass().PairList.Get(Pass().Offsets.Get(Slot) + LinearGroup));
        Texel = Tile * MATERIAL_CLASSIFY_TILE + uint2(Lane % MATERIAL_CLASSIFY_TILE, Lane / MATERIAL_CLASSIFY_TILE);
        if (Texel.x >= Pass().ScreenW || Texel.y >= Pass().ScreenH)
        {
            return;
        }

        // A tile is shared with other materials, so the slot classify stored keeps only this one's pixels.
        if ((uint)(Pass().SlotImage.Load(int2(Texel)).r + 0.5) != Slot + 1u)
        {
            return;
        }
    }

    const uint   Raw = Pass().VisBuffer.Load(int2(Texel));
    FVisTriangle T   = ResolveVisTriangle(Raw, Pass().DrawListCount);

    FGBufferData G = DefaultGBufferData();
    if (T.bValid)
    {
        // Loaded once, since a per-corner Ref.Inst fetch is a dependent PhysicalStorageBuffer load SPIR-V LICM will not merge.
        const float4x4 ModelMatrix    = T.Ref.ModelMatrix;
        float3x3       NormalMatrixWS = CalculateNormalMatrix(ModelMatrix);

        // WPO is reapplied per corner, with load and transform fused so only one corner's FVertexData is live.
        FGPUInstance* Inst = T.Ref.Inst;   // reached, not stored, see FMeshletRef
        const bool bWantVelocity = SPEC_DEFERRED_VELOCITY != 0u && Pass().Velocity.IsValid();
        const float4x4 PrevModel = bWantVelocity ? PrevModelMatrixOf(T.Ref.Inst, ModelMatrix) : ModelMatrix;

        FMeshletWorldVertex W[3];
        float3 FinalWorldPos[3];
        float3 PrevWorldPos[3];
        [unroll]
        for (int k = 0; k < 3; ++k)
        {
            FVertexData VertexData = LoadMeshletVertexData(T.Ref, T.Tri[k], true);
            W[k] = TransformMeshletVertex(VertexData, ModelMatrix, NormalMatrixWS);

            // Pose only; the offset the graph adds is applied below, once it has been evaluated at t-1.
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
                MaterialTimeOverride = GetPrevTime();

                WorldPos      = float4(PrevWorldPos[k], 1.0);
                NormalWS      = W[k].NormalWS;
                TangentWS     = W[k].TangentWS.xyz;
                TangentSignWS = W[k].TangentWS.w;

                FMaterialVertexInputs Material;
                $MATERIAL_VERTEX_INPUTS
                PrevWorldPos[k] += Material.WorldPositionOffset;

                MaterialTimeOverride = -1.0;
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
                Motion = PrevUV - ((float2(Texel) + 0.5) / ScreenSize - GetTemporalJitterUV());
            }

            Pass().Velocity[int2(Texel)] = float4(Motion, 0.0, 0.0);
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
        Input.WorldPosNoWPO = S.WorldPositionNoOffset;
        Input.UV            = S.UV;
        Input.InstanceIndex = T.Ref.InstanceID;
        Input.EntityID      = T.Ref.Inst.EntityID;
        Input.MaterialIndex = T.Ref.Inst.MaterialIndex;
        Input.InstanceFlags = (uint)T.Ref.InstFlags;
        Input.MeshletIndex  = T.Ref.GlobalMeshletIdx;
        // What a rasterized pixel shader would see here (pixel center, NDC z, 1/w).
        Input.Position      = float4(float2(Texel) + 0.5, ClipPos.z * InvW, InvW);

        // Locals named as the pixel graph codegen reads them.
        uint   EntityID       = Input.EntityID;
        uint   MaterialIndex  = Input.MaterialIndex;
        bool   bReceiveShadow = (Input.InstanceFlags & (uint)EInstanceFlags::ReceiveShadow) != 0u;
        float3 WorldPosition  = S.WorldPosition;
        float3 WorldPositionExcludingOffsets = S.WorldPositionNoOffset;
        float3 WorldNormal    = normalize(OrientedNormalWS);
        float4 WorldTangent   = Input.TangentWS;
        float2 UV0            = S.UV;
        float2 UV1            = S.UV1;
        float4 VertexColor    = S.VertexColor;
        float3 ViewPosition   = ViewPos.xyz;
        float2 UV0_DDX        = S.UVDDX;
        float2 UV0_DDY        = S.UVDDY;
        // No 2x2 quad exists in this lane, so set 1 carries its own barycentric gradients too.
        float2 UV1_DDX        = S.UV1DDX;
        float2 UV1_DDY        = S.UV1DDY;

        // Analytic gradients for the other UV sources, but none for WorldNormal since it is normalized and two-sided-flipped.
        float3 WorldPosition_DDX = InterpWeights(S.Bary.DDX, FinalWorldPos[0], FinalWorldPos[1], FinalWorldPos[2]);
        float3 WorldPosition_DDY = InterpWeights(S.Bary.DDY, FinalWorldPos[0], FinalWorldPos[1], FinalWorldPos[2]);
        float3 WorldPositionExcludingOffsets_DDX = InterpWeights(S.Bary.DDX, W[0].WorldPos, W[1].WorldPos, W[2].WorldPos);
        float3 WorldPositionExcludingOffsets_DDY = InterpWeights(S.Bary.DDY, W[0].WorldPos, W[1].WorldPos, W[2].WorldPos);
        float4 VertexColor_DDX   = InterpWeights4(S.Bary.DDX, W[0].Color, W[1].Color, W[2].Color);
        float4 VertexColor_DDY   = InterpWeights4(S.Bary.DDY, W[0].Color, W[1].Color, W[2].Color);

        // Not named WorldNormal_DDX so it stays out of the graph's alias set, and used only for specular AA.
        const float3 GeoNormalDDX = InterpWeights(S.Bary.DDX, W[0].NormalWS, W[1].NormalWS, W[2].NormalWS);
        const float3 GeoNormalDDY = InterpWeights(S.Bary.DDY, W[0].NormalWS, W[1].NormalWS, W[2].NormalWS);

        // Group-uniform so the wave ops in RequestMaterialResolution stay convergent.
        if ((LinearGroup + Pass().FeedbackPhase) % STREAMING_FEEDBACK_WINDOW == 0u)
        {
            RequestMaterialResolution(MaterialIndex, UV0_DDX, UV0_DDY, UV1_DDX, UV1_DDY);
        }
        $MATERIAL_INPUTS

        G = BuildGBufferData(Input, Material);

        // No quad in this lane, so the widening comes from surface curvature rather than the normal map.
        G.Roughness = ApplyGeometricSpecularAA(G.Roughness, GeoNormalDDX, GeoNormalDDY);
    }

    const FGBufferPacked P = PackGBuffer(G);
    Pass().GBufferA[int2(Texel)] = P.A;
    Pass().GBufferB[int2(Texel)] = P.B;
    Pass().GBufferC[int2(Texel)] = P.C;
    Pass().GBufferD[int2(Texel)] = P.D;
}
