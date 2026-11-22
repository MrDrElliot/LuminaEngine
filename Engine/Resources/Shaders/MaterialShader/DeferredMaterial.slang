// The compute lane has no rasterizer, so nothing here may use ddx/ddy or an implicit-derivative sample.
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Wind.slang"   // this template substitutes the material vertex token too (WPO reconstruction)
#include "Includes/DeferredMaterialCommon.slang"
#include "Includes/Impostor.slang"

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

    // The cull widened a thinned instance across the ground, so the previous pose takes the same widening or a still blade reports motion.
    FTransform3x4 Prev = PrevRetainedTransforms().Get(Slot);
    if (RetainedTransforms().Contains(Slot))
    {
        const float3 Retained   = RetainedTransforms().Get(Slot).Row0.xyz;
        const float  RetainedSq = dot(Retained, Retained);
        const float3 Drawn      = Inst.Transform.Row0.xyz;
        const float  Widen      = RetainedSq > 0.0 ? sqrt(dot(Drawn, Drawn) / RetainedSq) : 1.0;
        Prev.Row0.xyz *= Widen;
        Prev.Row2.xyz *= Widen;
    }
    return Prev.ToMatrix();
}

[shader("compute")]
[numthreads(MATERIAL_PIXEL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    uint2 Texel;
    uint  LinearGroup;
    if (!FindDeferredTexel(GroupID, Lane, Texel, LinearGroup))
    {
        return;
    }

    const uint   Raw = Pass().VisBuffer.Load(int2(Texel));
    FVisTriangle T   = ResolveVisTriangle(Raw, Pass().DrawListCount);

    FGBufferData G = DefaultGBufferData();
    if (IsVisProxy(Raw))
    {
        // A far proxy has no triangle, so the splat's own averaged attributes stand in for the interpolated surface.
        const FVisProxy P = ResolveVisProxy(Raw);
        if (P.bValid)
        {
            const float2 ScreenSize = float2(Pass().ScreenW, Pass().ScreenH);
            const float3 LocalNormal = UnpackNormal(P.Proxy.PackedNormal);
            float3 NormalWS = mul(CalculateNormalMatrix(P.Model), LocalNormal);
            NormalWS = dot(NormalWS, NormalWS) > 1e-12 ? normalize(NormalWS) : float3(0.0, 1.0, 0.0);

            // A pixel-sized splat faces the camera more than its averaged normal does, so the normal leans toward the eye.
            float3 GroundWiden;
            float3 WorldPosition = ProxyWidenedCenter(P.Model, P.Proxy, GroundWiden);
            const float3 ToEye = IsCameraOrthographic() ? -GetCameraForward() : normalize(GetCameraPosition() - WorldPosition);
            NormalWS = normalize(lerp(NormalWS, ToEye, 0.5));

            // An impostor's own views carry the surface this texel sees, which replace the splat's averages once the material has run.
            FMeshletHeader* Header    = MeshletHeaderAt(P.Static.MeshletHeaderSlot);
            const bool      bImpostor = Header.bHasImpostor;
            FImpostorSample Impostor  = (FImpostorSample)0;
            if (bImpostor)
            {
                const float4x4 Model   = ProxyWidenedModel(P.Model, GroundWiden);
                // Unjittered by the inverse projection itself, which carries the jitter the raster drew with.
                const float2   PixelUV = (float2(Texel) + 0.5) / ScreenSize;
                const float3   Center  = mul(Model, float4(Header.ImpostorCenter, 1.0)).xyz;
                const float3   Forward = GetCameraForward();
                // A perspective ray is rotated out of view space, since the near point minus the camera keeps too few bits to aim at a far quad.
                const float3 RayStart  = IsCameraOrthographic() ? ReconstructWorldPos(PixelUV, 1.0) : GetCameraPosition();
                const float3 PixelRay  = IsCameraOrthographic() ? Forward : GetCameraWorldRayDir(PixelUV);
                // Moved along the pixel's ray to the plane the raster's quad lies in, so the mesh-local origin stays small at any distance.
                const float3 QuadPoint = RayStart + PixelRay * (dot(Center - RayStart, Forward) / max(dot(PixelRay, Forward), 1.0e-4));
                const FImpostorRay   Ray   = MakeImpostorRay(Header, Model, QuadPoint);
                const FImpostorViews Views = SelectImpostorViews(Ray.ToEye, Header.ImpostorFrames);
                const float3 Scale  = GetInstanceScale(P.Model);
                const float  Mip    = ImpostorMipLevel(Header, Center, Header.ImpostorRadius * max(Scale.x, max(Scale.y, Scale.z)));
                Impostor = SampleImpostor(Header, Views, Ray.Origin, Ray.Dir, Mip);
                // A pixel the raster kept can still miss every texel here, and the view's average beats the black an empty sample normalizes to.
                if (Impostor.Coverage < kImpostorMissCoverage)
                {
                    Impostor = SampleImpostor(Header, Views, Ray.Origin, Ray.Dir, log2(float(Header.ImpostorFrameSize)));
                }

                const float3 ImpostorNormal = mul(CalculateNormalMatrix(Model), Impostor.Normal);
                NormalWS = dot(ImpostorNormal, ImpostorNormal) > 1e-12 ? normalize(ImpostorNormal) : NormalWS;

                // The baked surface on this texel's ray, the point the raster wrote the depth of.
                WorldPosition = mul(Model, float4(Ray.Origin + Ray.Dir * Impostor.RayT, 1.0)).xyz;
            }

            const float3 Helper    = abs(NormalWS.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
            const float3 TangentWS = normalize(cross(Helper, NormalWS));

            const float4 ViewPos = CameraViewPosition(WorldPosition);
            const float4 ClipPos = CameraClipPosition(ViewPos);
            const float  InvW    = 1.0 / max(ClipPos.w, 1e-6);
            const float2 UV      = UnpackHalf2(P.Proxy.PackedUV);

            FBaseVertexOutput Input;
            Input.FragColor     = UnpackColor(P.Proxy.Color);
            Input.NormalVS      = float4(normalize(NormalWorldToView(NormalWS)), 1.0);
            Input.NormalWS      = float4(NormalWS, 1.0);
            Input.TangentWS     = float4(TangentWS, 1.0);
            Input.FragPos       = ViewPos;
            Input.WorldPosNoWPO = WorldPosition;
            Input.UV            = UV;
            Input.UV1           = UV;
            Input.InstanceIndex = 0u;
            Input.EntityID      = P.Static.EntityID;
            Input.MaterialIndex = P.Static.MaterialIndex;
            Input.InstanceFlags = (uint)EInstanceFlags::ReceiveShadow;
            Input.MeshletIndex  = 0u;
            Input.Position      = float4(float2(Texel) + 0.5, ClipPos.z * InvW, InvW);

            uint   EntityID       = Input.EntityID;
            uint   MaterialIndex  = Input.MaterialIndex;
            bool   bReceiveShadow = true;
            float3 WorldPositionExcludingOffsets = WorldPosition;
            float3 WorldNormal    = NormalWS;
            float4 WorldTangent   = Input.TangentWS;
            float2 UV0            = UV;
            float2 UV1            = UV;
            float4 VertexColor    = Input.FragColor;
            float3 ViewPosition   = ViewPos.xyz;
            // The whole texture across a couple of pixels, so every sample lands on its coarsest mips.
            float2 UV0_DDX        = float2(0.5, 0.0);
            float2 UV0_DDY        = float2(0.0, 0.5);
            float2 UV1_DDX        = UV0_DDX;
            float2 UV1_DDY        = UV0_DDY;
            float3 WorldPosition_DDX = float3(0.0, 0.0, 0.0);
            float3 WorldPosition_DDY = float3(0.0, 0.0, 0.0);
            float3 WorldPositionExcludingOffsets_DDX = WorldPosition_DDX;
            float3 WorldPositionExcludingOffsets_DDY = WorldPosition_DDY;
            float4 VertexColor_DDX   = float4(0.0, 0.0, 0.0, 0.0);
            float4 VertexColor_DDY   = float4(0.0, 0.0, 0.0, 0.0);

            if ((LinearGroup + Pass().FeedbackPhase) % STREAMING_FEEDBACK_WINDOW == 0u)
            {
                RequestMaterialResolution(MaterialIndex, UV0_DDX, UV0_DDY, UV1_DDX, UV1_DDY);
            }
            $MATERIAL_INPUTS

            // Velocity stays as the camera motion the Velocity pass derived from this proxy's depth.
            G = BuildGBufferData(Input, Material);
            if (bImpostor)
            {
                G.BaseColor        = Impostor.BaseColor;
                G.WorldNormal      = NormalWS;
                G.AmbientOcclusion = Impostor.AmbientOcclusion;
            }
        }
    }
    else if (T.bValid)
    {
        // Loaded once, since a per-corner Ref.Inst fetch is a dependent PhysicalStorageBuffer load SPIR-V LICM will not merge.
        const float4x4 ModelMatrix    = T.Ref.ModelMatrix;
        float3x3       NormalMatrixWS = CalculateNormalMatrix(ModelMatrix);

        // WPO is reapplied per corner, with load and transform fused so only one corner's FVertexData is live.
        FGPUInstance* Inst = T.Ref.Inst;   // reached, not stored, see FMeshletRef
        const bool bWantVelocity = SPEC_DEFERRED_VELOCITY != 0u && Pass().Velocity.IsValid();
        const float4x4 PrevModel = bWantVelocity ? PrevModelMatrixOf(T.Ref.Inst, ModelMatrix) : ModelMatrix;
        // The same bit the visbuffer raster read, or the rebuilt triangle would not be the one on screen.
        const bool bApplyWPO = Inst.bApplyWPO;

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

            FinalWorldPos[k] = W[k].WorldPos;
            if (bApplyWPO)
            {
                FMaterialVertexInputs Material;
                $MATERIAL_VERTEX_INPUTS
                FinalWorldPos[k] += Material.WorldPositionOffset;
            }

            // A constant offset folds away here, so only a graph with real WPO pays for this second pass.
            if (bWantVelocity && bApplyWPO)
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

        const float3 CornerNormals[3] = { W[0].NormalWS, W[1].NormalWS, W[2].NormalWS };
        const float3 OrientedNormalWS = OrientVisNormal(T.Ref, S.NormalWS, S.WorldPosition, FinalWorldPos, CornerNormals);
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
        ApplyGrassShading(T.Ref, ModelMatrix, S.WorldPositionNoOffset, G.WorldNormal, G.Roughness, G.AmbientOcclusion);

        // No quad in this lane, so the widening comes from surface curvature rather than the normal map.
        G.Roughness = ApplyGeometricSpecularAA(G.Roughness, GeoNormalDDX, GeoNormalDDY);
        G.ClearcoatRoughness = ApplyGeometricSpecularAA(G.ClearcoatRoughness, GeoNormalDDX, GeoNormalDDY);
    }

    WriteDeferredGBuffer(Texel, G);
}
