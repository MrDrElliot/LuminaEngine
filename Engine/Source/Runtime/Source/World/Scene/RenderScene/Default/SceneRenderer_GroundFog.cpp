#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "World/Entity/Components/GroundFogComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    namespace GroundFogInternal
    {
        TConsoleVar<bool> CVarGroundFog(
            "r.GroundFog",
            true,
            "Renders Ground Fog components. Off skips the march, the flow and the obstacle view.");

        TConsoleVar<int32> CVarGroundFogSteps(
            "r.GroundFog.Steps",
            12,
            "Samples each view ray takes through a ground fog layer.");

        TConsoleVar<int32> CVarGroundFogSimResolution(
            "r.GroundFog.SimResolution",
            256,
            "Texels across the flow that parts the ground fog around the camera.");

        TConsoleVar<float> CVarGroundFogSimExtent(
            "r.GroundFog.SimExtent",
            20.0f,
            "Half width in meters of the flow that parts the ground fog around the camera.");

        TConsoleVar<int32> CVarGroundFogPressureIterations(
            "r.GroundFog.PressureIterations",
            12,
            "Relaxation sweeps that keep the flow from compressing the fog. Warm started, so a few go a long way.");

        // Fixed so a wake spreads at the same speed whatever the frame rate.
        constexpr float  kStepRate               = 60.0f;
        constexpr uint32 kMaxStepsPerFrame       = 4u;
        // Long enough for a parted path to fill back in before the flow's images are released.
        constexpr uint32 kIdleReleaseFrames      = 600u;
        // Koschmieder extinction ln(50)/V, where contrast falls to 2%.
        constexpr float  kContrastThreshold      = 3.912f;
        // The obstacle camera sits this far under the ground so a body standing on it is not clipped.
        constexpr float  kObstacleBelow          = 2.0f;
        constexpr float  kObstacleAbove          = 0.25f;
        // Turns the swirl slider into the confinement gain the flow uses per unit of curl.
        constexpr float  kSwirlGain              = 8.0f;
        // Texels the flow may cross in one step before the semi-Lagrangian trace starts to smear.
        constexpr float  kMaxTexelsPerStep       = 3.0f;
        constexpr uint32 kSimGroupSize           = 8u;
        constexpr uint32 kMarchGroupSize         = 8u;

        enum ESimStage : uint32
        {
            Advect     = 0,
            Divergence = 1,
            Jacobi     = 2,
            Project    = 3,
        };

        struct FGroundFogSimPC
        {
            RHI::TGPUSpan<FGPUGroundFogSim> Params;
            uint32 Stage;
            uint32 FlowIn;
            uint32 FillIn;
            uint32 FlowOut;
            uint32 FillOut;
            uint32 PressureIn;
            uint32 PressureOut;
            uint32 DivCurlIn;
            uint32 DivCurlOut;
            uint32 Obstacles;
            uint32 _Pad0;
            uint32 _Pad1;
        };
        static_assert(sizeof(FGroundFogSimPC) == 64, "FGroundFogSimPC must match GroundFogSim.slang.");

        struct FGroundFogMarchPC
        {
            uint32 Depth;
            uint32 Output;
            uint32 OutputDepth;
            uint32 ScreenW;

            uint32 ScreenH;
            uint32 TraceW;
            uint32 TraceH;
            uint32 Steps;

            RHI::TGPUSpan<FGPUGroundFogLayer> Layers;

            uint32   Flow;
            uint32   Fill;
            FVector2 SimOrigin;

            float  SimInvWidth;
            float  Time;
            uint32 _Pad0;
            uint32 _Pad1;
        };
        static_assert(sizeof(FGroundFogMarchPC) == 80, "FGroundFogMarchPC must match GroundFogMarch.slang.");

        FVector4 MatrixRow(const FMatrix4& Matrix, int32 Row)
        {
            return FVector4(Matrix[0][Row], Matrix[1][Row], Matrix[2][Row], Matrix[3][Row]);
        }

        // Horizontal distance from a point to a layer's area, zero inside it.
        float DistanceToArea(const FGPUGroundFogLayer& Layer, const FVector2& Extent, const FVector3& Point)
        {
            const FVector4 P(Point, 1.0f);
            const float X = Math::Max(Math::Abs(Math::Dot(Layer.WorldToLayerX, P)) - 1.0f, 0.0f) * Extent.x;
            const float Z = Math::Max(Math::Abs(Math::Dot(Layer.WorldToLayerZ, P)) - 1.0f, 0.0f) * Extent.y;
            return Math::Sqrt(X * X + Z * Z);
        }

        void EnsureImage(FSceneImage& Image, uint32 Resolution, EFormat Format, const char* Name)
        {
            if (Image.IsValid() && Image.GetSizeX() == Resolution && Image.Desc.Format == Format)
            {
                return;
            }
            RetireSceneImage(Image);

            RHI::FTextureDesc Desc;
            Desc.Type      = RHI::ETextureType::Tex2D;
            Desc.Dimension = FUIntVector3(Resolution, Resolution, 1);
            Desc.Format    = Format;
            Desc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage | RHI::EImageUsageFlags::TransferDst;
            Image = CreateSceneImage(Desc, true, true);
            RHI::SetDebugName(Image.Texture, Name);
        }
    }

    void FDefaultSceneRenderer::ExtractGroundFog(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        using namespace GroundFogInternal;

        FFrameData::FGroundFog& Out = Frame.GroundFog;
        Out.Layers.Reset();
        Out.bSimulate               = false;
        Out.bReset                  = false;
        Out.Steps                   = 0u;
        Out.ObstacleShadowDataIndex = Constants::kIndexNone;
        Out.ObstacleCullView        = Constants::kIndexNoneU32;

        FGroundFogState& State = GroundFogState;
        const auto MarkIdle = [&State]()
        {
            State.IdleFrames = State.IdleFrames == ~0u ? ~0u : State.IdleFrames + 1u;
        };

        const ECS::TComponentStorage<SGroundFogComponent> Storage = Registry.FindStorage<SGroundFogComponent>();
        if (!CVarGroundFog.GetValue() || !Storage || Storage.IsEmpty())
        {
            MarkIdle();
            return;
        }

        LUMINA_PROFILE_SECTION("Ground Fog Extract");

        const FVector3 Camera = ExtractFrame->ViewVolume.GetViewPosition();
        const float    Time   = Frame.SceneGlobalData.Time;
        const ECS::TComponentStorage<STransformComponent> Transforms = Registry.GetStorage<STransformComponent>();

        const SGroundFogComponent* Driver = nullptr;
        float DriverBase     = 0.0f;
        float DriverDistance = std::numeric_limits<float>::max();

        Registry.View<ECS::Read<SGroundFogComponent>, ECS::Without<SDisabledTag>>().ForEach([&](ECS::FEntity Entity, const SGroundFogComponent& Fog)
        {
            if (!Fog.bEnabled || Fog.Thickness <= 0.0f || Fog.VisibilityDistance <= 0.0f)
            {
                return;
            }
            const STransformComponent* Transform = Transforms.TryGet(Entity);
            if (Transform == nullptr)
            {
                return;
            }
            if (static_cast<uint32>(Out.Layers.Num()) >= GGroundFogMaxLayers)
            {
                LOG_WARN_ONCE("[Rendering] More than {} ground fog layers reach the view (entity {} is dropped), so merge or disable the extra layers.", GGroundFogMaxLayers, Entity.GetPacked());
                return;
            }

            const FVector2 Extent(Math::Max(Fog.Extent.x, 0.5f), Math::Max(Fog.Extent.y, 0.5f));
            const FMatrix4 WorldToLayer = Math::Inverse(Math::Scale(Transform->GetWorldMatrixCached(), FVector3(Extent.x, 1.0f, Extent.y)));
            const float    Base         = Transform->GetWorldLocationCached().y;
            const float    ViewDistance = Math::Max(Fog.ViewDistance, 1.0f);

            FGPUGroundFogLayer Layer;
            Layer.WorldToLayerX = MatrixRow(WorldToLayer, 0);
            Layer.WorldToLayerZ = MatrixRow(WorldToLayer, 2);

            const float Distance = DistanceToArea(Layer, Extent, Camera);
            if (Distance > ViewDistance)
            {
                return;
            }

            const float EdgeFade = Math::Max(Fog.EdgeFade, 0.01f);
            Layer.Shape  = FVector4(Base, Fog.Thickness, Math::Clamp(Fog.TopSoftness, 0.01f, 1.0f), kContrastThreshold / Math::Max(Fog.VisibilityDistance, 0.1f));
            Layer.Albedo = FVector4(Fog.Albedo, Math::Max(Fog.ScatteringIntensity, 0.0f));
            Layer.Edge   = FVector4(EdgeFade / Extent.x, EdgeFade / Extent.y, ViewDistance, Math::Clamp(Fog.Anisotropy, -0.9f, 0.9f));
            Layer.Wisps  = FVector4(Math::Saturate(Fog.WispStrength), 1.0f / Math::Max(Fog.WispScale, 0.1f), Fog.Wind.x * Time, Fog.Wind.y * Time);
            Out.Layers.Emplace(Layer);

            if (Fog.bInteractive && Distance < DriverDistance)
            {
                Driver         = &Fog;
                DriverBase     = Base;
                DriverDistance = Distance;
            }
        });

        const uint32 Resolution = (uint32)Math::Clamp(CVarGroundFogSimResolution.GetValue(), 64, 1024) & ~(kSimGroupSize - 1u);
        const float  HalfExtent = Math::Max(CVarGroundFogSimExtent.GetValue(), 4.0f);
        const float  TexelWorld = HalfExtent * 2.0f / (float)Resolution;

        if (Driver == nullptr || DriverDistance > HalfExtent)
        {
            MarkIdle();
            return;
        }

        Out.bSimulate = true;
        Out.bReset    = State.IdleFrames != 0u || Resolution != State.Resolution || TexelWorld != State.TexelWorld;
        if (Out.bReset)
        {
            State.Accumulator = 0.0f;
            State.Parity      = 0u;
        }
        State.IdleFrames = 0u;

        const FIntVector2 OriginTexel((int32)Math::Floor((Camera.x - HalfExtent) / TexelWorld),
                                      (int32)Math::Floor((Camera.z - HalfExtent) / TexelWorld));

        State.Accumulator += Math::Clamp(Frame.SceneGlobalData.DeltaTime, 0.0f, 0.1f);
        uint32 Steps = Math::Min((uint32)(State.Accumulator * kStepRate), kMaxStepsPerFrame);
        State.Accumulator = Math::Max(State.Accumulator - (float)Steps / kStepRate, 0.0f);
        // A cleared state has to be written once before the march can read it.
        Steps = Out.bReset ? Math::Max(Steps, 1u) : Steps;

        FGPUGroundFogSim& Sim = Out.Sim;
        if (Steps > 0u)
        {
            Sim.Shift         = Out.bReset ? FIntVector2(0) : OriginTexel - State.OriginTexel;
            State.OriginTexel = OriginTexel;
            State.Resolution  = Resolution;
            State.TexelWorld  = TexelWorld;
        }

        const float Dt = 1.0f / kStepRate;
        Sim.Origin       = FVector2((float)State.OriginTexel.x, (float)State.OriginTexel.y) * State.TexelWorld;
        Sim.TexelWorld   = State.TexelWorld;
        Sim.Resolution   = State.Resolution;
        Sim.Dt           = Dt;
        Sim.bIngest      = 1u;
        Sim.BandBottom   = DriverBase;
        Sim.BandTop      = DriverBase + Driver->Thickness;
        Sim.Wind         = Driver->Wind;
        Sim.VelocityKeep = Math::Exp(-Dt / Math::Max(Driver->FlowLifetime, 0.05f));
        Sim.SwirlKeep    = Math::Exp(-Dt / Math::Max(Driver->FlowLifetime * 2.0f, 0.1f));
        Sim.RefillRate   = 1.0f - Math::Exp(-Dt / Math::Max(Driver->RefillTime, 0.1f));
        Sim.Displacement = Driver->DisplacementStrength;
        Sim.Vorticity    = Driver->Swirl * kSwirlGain;
        Sim.MaxSpeed     = kMaxTexelsPerStep * State.TexelWorld / Dt;

        Out.Steps            = Steps;
        Out.JacobiIterations = (uint32)Math::Clamp(CVarGroundFogPressureIterations.GetValue(), 1, 64);
        Out.InputParity      = State.Parity;
        Out.OutputParity     = State.Parity ^ (Steps & 1u);
        State.Parity         = Out.OutputParity;

        if (Steps == 0u || Driver->DisplacementStrength <= 0.0f)
        {
            return;
        }

        const uint32 ShadowSlot = Frame.Lighting.ShadowDataCount.fetch_add(1, std::memory_order_acquire);
        if (ShadowSlot >= (uint32)MAX_SHADOWS)
        {
            return;
        }

        // Looks up from under the ground, so the nearest surface each texel sees is the lowest point of whatever stands in the fog there.
        const FVector2 Center = Sim.Origin + FVector2(HalfExtent);
        const float    EyeY   = Sim.BandBottom - kObstacleBelow;
        const float    Range  = Sim.BandTop + kObstacleAbove - EyeY;
        const FVector3 Eye(Center.x, EyeY, Center.y);
        const FMatrix4 View = Math::LookAt(Eye, Eye + FVector3(0.0f, 1.0f, 0.0f), FVector3(0.0f, 0.0f, 1.0f));
        FMatrix4 Projection = Math::Ortho(-HalfExtent, HalfExtent, -HalfExtent, HalfExtent, 0.0f, Range);
        Projection[1][1] *= -1.0f;
        const FMatrix4 ViewProjection = Projection * View;

        FLightShadowData& ShadowData = Frame.Lighting.Shadows[ShadowSlot];
        ShadowData.ViewProjection[0] = ViewProjection;
        ShadowData.Shadow[0] = {};
        ShadowData.Shadow[0].AtlasUVScale    = FVector2(1.0f);
        ShadowData.Shadow[0].ShadowMapIndex  = Constants::kIndexNone;
        ShadowData.Shadow[0].LightIndex      = Constants::kIndexNone;
        ShadowData.Shadow[0].ShadowDataIndex = (int32)ShadowSlot;

        Out.ObstacleShadowDataIndex = (int32)ShadowSlot;
        Out.ObstacleViewProjection  = ViewProjection;

        // The view only looks along Y, so x and z map to the obstacle map's u and v and Y alone sets its depth.
        Sim.ObstacleU = FVector4(0.5f * ViewProjection[0][0], 0.5f * ViewProjection[2][0], 0.5f * ViewProjection[3][0] + 0.5f, 0.0f);
        Sim.ObstacleV = FVector4(0.5f * ViewProjection[0][1], 0.5f * ViewProjection[2][1], 0.5f * ViewProjection[3][1] + 0.5f, 0.0f);
        Sim.DepthToY  = FVector2(1.0f / ViewProjection[1][2], -ViewProjection[3][2] / ViewProjection[1][2]);
    }

    void FDefaultSceneRenderer::GroundFogObstaclePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const FFrameData::FGroundFog& Fog = Frame.GroundFog;
        FGroundFogState& State = GroundFogState;
        if (Fog.ObstacleCullView == Constants::kIndexNoneU32 || Fog.Steps == 0u)
        {
            return;
        }

        if (!State.Obstacles.IsValid() || State.Obstacles.GetSizeX() != Fog.Sim.Resolution)
        {
            RetireSceneImage(State.Obstacles);

            RHI::FTextureDesc Desc;
            Desc.Type      = RHI::ETextureType::Tex2D;
            Desc.Dimension = FUIntVector3(Fog.Sim.Resolution, Fog.Sim.Resolution, 1);
            Desc.Format    = EFormat::D32;
            Desc.Usage     = RHI::EImageUsageFlags::DepthAttachment | RHI::EImageUsageFlags::Sampled;
            State.Obstacles = CreateSceneImage(Desc);
            RHI::SetDebugName(State.Obstacles.Texture, "GroundFog.Obstacles");
        }

        SCENE_GPU_SCOPE(CL, "Ground Fog Obstacles");

        const FUIntVector2 Extent = State.Obstacles.GetExtent();

        RHI::FRenderPassDesc Pass;
        Pass.DepthAttachment.Texture  = State.Obstacles.Texture;
        Pass.DepthAttachment.LoadOp   = RHI::ELoadOp::Clear;
        Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
        Pass.DepthAttachment.Color[0] = 1.0f;
        Pass.RenderArea               = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest = RHI::EOp::Less;
        RHI::CmdSetDepthStencil(CL, DepthDesc);

        // Both faces, since from below the surface facing away from the camera is often the lowest one.
        for (uint32 OpaqueIdx : Frame.Geometry.OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = Frame.Geometry.DrawCommands[OpaqueIdx];
            const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, FShaderH{}, nullptr, EFormat::D32, /*bBothFaces*/ true);
            DrawShadowBatch(CL, Batch, bUseMesh, Fog.ObstacleCullView, Fog.ObstacleShadowDataIndex, 0, Extent);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::GroundFogSimPass(RHI::FCmdListH CL)
    {
        using namespace GroundFogInternal;

        const FFrameData::FGroundFog& Fog = RenderFrame->GroundFog;
        FGroundFogState& State = GroundFogState;
        if (!Fog.bSimulate || Fog.Steps == 0u)
        {
            return;
        }

        static const FShaderH SimShader = FShaderLibrary::Get("GroundFogSim.slang");
        if (!SimShader)
        {
            return;
        }

        const uint32 Resolution = Fog.Sim.Resolution;
        EnsureImage(State.Flow[0],     Resolution, EFormat::RGBA16_FLOAT, "GroundFog.Flow0");
        EnsureImage(State.Flow[1],     Resolution, EFormat::RGBA16_FLOAT, "GroundFog.Flow1");
        EnsureImage(State.Fill[0],     Resolution, EFormat::RGBA16_FLOAT, "GroundFog.Fill0");
        EnsureImage(State.Fill[1],     Resolution, EFormat::RGBA16_FLOAT, "GroundFog.Fill1");
        EnsureImage(State.Pressure[0], Resolution, EFormat::R16_FLOAT,    "GroundFog.Pressure0");
        EnsureImage(State.Pressure[1], Resolution, EFormat::R16_FLOAT,    "GroundFog.Pressure1");
        EnsureImage(State.DivCurl,     Resolution, EFormat::RG16_FLOAT,   "GroundFog.DivCurl");

        SCENE_GPU_SCOPE(CL, "Ground Fog Flow");

        if (Fog.bReset)
        {
            // Rest is still air with the layer standing at its full thickness and nothing in it.
            constexpr float Still[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            constexpr float Full[4]  = { 1.0f, 0.0f, 0.0f, 0.0f };
            for (uint32 Index = 0; Index < 2u; ++Index)
            {
                RHI::CmdClearTexture(CL, State.Flow[Index].Texture, Still);
                RHI::CmdClearTexture(CL, State.Fill[Index].Texture, Full);
                RHI::CmdClearTexture(CL, State.Pressure[Index].Texture, Still);
            }
            State.PressureParity = 0u;
            Barriers::TransferToCompute(CL);
        }

        const uint32 Obstacles = Fog.ObstacleCullView != Constants::kIndexNoneU32 && State.Obstacles.IsValid()
                               ? (uint32)State.Obstacles.GetResourceID() : Constants::kIndexNoneU32;
        const uint32 Groups    = Resolution / kSimGroupSize;

        FGPUGroundFogSim FirstStep = Fog.Sim;
        // A cleared state has no last occupancy, so whatever already stands in the fog would read as having just stepped in.
        FirstStep.Displacement = Fog.bReset ? 0.0f : Fog.Sim.Displacement;
        FGPUGroundFogSim LaterStep = Fog.Sim;
        LaterStep.Shift   = FIntVector2(0);
        LaterStep.bIngest = 0u;
        const RHI::TGPUSpan<FGPUGroundFogSim> FirstParams(RHI::CopyTransientArray(&FirstStep, 1));
        const RHI::TGPUSpan<FGPUGroundFogSim> LaterParams(RHI::CopyTransientArray(&LaterStep, 1));

        const auto Dispatch = [&](FGroundFogSimPC& PC)
        {
            DispatchCompute(CL, SimShader, PC, Groups, Groups);
            RHI::Barriers::ComputeToCompute(CL);
        };

        uint32 Input = Fog.InputParity;
        for (uint32 Step = 0; Step < Fog.Steps; ++Step)
        {
            const uint32 Output = Input ^ 1u;

            FGroundFogSimPC PC = {};
            PC.Params    = Step == 0u ? FirstParams : LaterParams;
            PC.Obstacles = Step == 0u ? Obstacles : Constants::kIndexNoneU32;

            PC.Stage   = ESimStage::Advect;
            PC.FlowIn  = (uint32)State.Flow[Input].GetResourceID();
            PC.FillIn  = (uint32)State.Fill[Input].GetResourceID();
            PC.FlowOut = (uint32)State.Flow[Output].GetMipUAVIndex(0);
            PC.FillOut = (uint32)State.Fill[Output].GetMipUAVIndex(0);
            Dispatch(PC);

            PC.Stage      = ESimStage::Divergence;
            PC.FlowIn     = (uint32)State.Flow[Output].GetResourceID();
            PC.FillIn     = (uint32)State.Fill[Output].GetResourceID();
            PC.DivCurlOut = (uint32)State.DivCurl.GetMipUAVIndex(0);
            Dispatch(PC);

            PC.Stage     = ESimStage::Jacobi;
            PC.DivCurlIn = (uint32)State.DivCurl.GetResourceID();
            for (uint32 Iteration = 0; Iteration < Fog.JacobiIterations; ++Iteration)
            {
                PC.PressureIn  = (uint32)State.Pressure[State.PressureParity].GetResourceID();
                PC.PressureOut = (uint32)State.Pressure[State.PressureParity ^ 1u].GetMipUAVIndex(0);
                Dispatch(PC);
                State.PressureParity ^= 1u;
            }

            PC.Stage      = ESimStage::Project;
            PC.PressureIn = (uint32)State.Pressure[State.PressureParity].GetResourceID();
            Dispatch(PC);

            Input = Output;
        }

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                        RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderRead);
    }

    void FDefaultSceneRenderer::GroundFogMarchPass(RHI::FCmdListH CL)
    {
        using namespace GroundFogInternal;

        AtmosphereTerms.GroundFogIndex      = Constants::kIndexNoneU32;
        AtmosphereTerms.GroundFogDepthIndex = Constants::kIndexNoneU32;

        const FFrameData& Frame = *RenderFrame;
        const FFrameData::FGroundFog& Fog = Frame.GroundFog;
        if (Fog.Layers.IsEmpty())
        {
            return;
        }

        static const FShaderH CS = FShaderLibrary::Get("GroundFogMarch.slang");
        const FSceneImage& Output      = GetNamedImage(ENamedImage::GroundFog);
        const FSceneImage& OutputDepth = GetNamedImage(ENamedImage::GroundFogDepth);
        if (!CS || !Output.IsValid() || !OutputDepth.IsValid() || Output.GetMipUAVIndex(0) < 0 || OutputDepth.GetMipUAVIndex(0) < 0)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Ground Fog");

        const FGroundFogState& State = GroundFogState;
        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);
        const uint32 FlowParity = Fog.OutputParity;
        const bool   bFlow      = Fog.bSimulate && State.Flow[FlowParity].IsValid() && State.Fill[FlowParity].IsValid();

        FGroundFogMarchPC PC = {};
        PC.Depth       = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        PC.Output      = (uint32)Output.GetMipUAVIndex(0);
        PC.OutputDepth = (uint32)OutputDepth.GetMipUAVIndex(0);
        PC.ScreenW     = HDR.GetSizeX();
        PC.ScreenH     = HDR.GetSizeY();
        PC.TraceW      = Output.GetSizeX();
        PC.TraceH      = Output.GetSizeY();
        PC.Steps       = (uint32)Math::Clamp(CVarGroundFogSteps.GetValue(), 2, 64);
        PC.Layers      = RHI::TGPUSpan<FGPUGroundFogLayer>(RHI::CopyTransientArray(Fog.Layers.GetData(), Fog.Layers.Num()));
        PC.Flow        = bFlow ? (uint32)State.Flow[FlowParity].GetResourceID() : Constants::kIndexNoneU32;
        PC.Fill        = bFlow ? (uint32)State.Fill[FlowParity].GetResourceID() : Constants::kIndexNoneU32;
        PC.SimOrigin   = Fog.Sim.Origin;
        PC.SimInvWidth = 1.0f / Math::Max(Fog.Sim.TexelWorld * (float)Math::Max(Fog.Sim.Resolution, 1u), 1.0e-3f);
        PC.Time        = Frame.SceneGlobalData.Time;

        DispatchCompute(CL, CS, PC,
                        RenderUtils::GetGroupCount(PC.TraceW, kMarchGroupSize),
                        RenderUtils::GetGroupCount(PC.TraceH, kMarchGroupSize), 1);

        RHI::Barriers::ComputeToCompute(CL);

        AtmosphereTerms.GroundFogIndex      = (uint32)Output.GetResourceID();
        AtmosphereTerms.GroundFogDepthIndex = (uint32)OutputDepth.GetResourceID();
    }

    void FDefaultSceneRenderer::ReleaseIdleGroundFogResources()
    {
        using namespace GroundFogInternal;

        FGroundFogState& State = GroundFogState;
        if (State.IdleFrames < kIdleReleaseFrames || !State.Flow[0].IsValid())
        {
            return;
        }

        for (uint32 Index = 0; Index < 2u; ++Index)
        {
            RetireSceneImage(State.Flow[Index]);
            RetireSceneImage(State.Fill[Index]);
            RetireSceneImage(State.Pressure[Index]);
        }
        RetireSceneImage(State.DivCurl);
        RetireSceneImage(State.Obstacles);
    }
}
