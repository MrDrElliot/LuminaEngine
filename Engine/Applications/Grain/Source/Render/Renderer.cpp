#include "Renderer.h"
#include "Shaders.h"

#include "Log/Log.h"
#include "Renderer/RHICore.h"
#include "Renderer/ShaderCompiler.h"
#include "Tools/Image/ImageWrite.h"

#include <cstring>

namespace Grain
{
    namespace
    {
        constexpr EFormat kSceneFormat = EFormat::RGBA16_FLOAT;

        // One slot per pass boundary, so a report is a walk over consecutive differences.
        constexpr const char* kTimerNames[] =
        {
            "Water sim", "Destroy", "Raymarch", "Temporal", "A trous", "Compose", "Bloom", "Composite",
        };

        constexpr uint32 kTimerSlots = 9;

        struct FBloomArgs
        {
            uint32   SourceID;
            uint32   bFirstPass;
            FVector2 SourceTexelSize;
            float    Threshold;
            float    Radius;
            float    Intensity;
            float    Pad0;
        };

        struct FSimArgs
        {
            RHI::GPUPtr Grid;
            RHI::GPUPtr Coarse;
            uint32      Source[4];
            uint32      Control[4];
        };

        static_assert(sizeof(FSimArgs) == 48, "Slang mirror expects a packed 48 byte block.");

        struct FCompositeArgs
        {
            uint32   SceneID;
            uint32   BloomID;
            FVector2 Resolution;
            float    BloomIntensity;
            float    Exposure;
            float    Vignette;
            float    Pad0;
        };

        static_assert(sizeof(FBloomArgs) == 32, "Slang mirror expects a packed 32 byte block.");
        static_assert(sizeof(FCompositeArgs) == 32, "Slang mirror expects a packed 32 byte block.");

        static_assert(kRootCountX == 5 && kRootCountY == 4 && kRootCountZ == 5,
            "Shaders.h hardcodes the root grid, so both sides have to change together.");
        static_assert(kVoxelsPerRoot == 512, "Shaders.h hardcodes kRootSpan.");

        TVector<uint32> CompileFrom(const char* Module, const char* EntryPoint, const char* DebugName)
        {
            TVector<uint32> Spirv;

            FShaderCompileOptions Options;
            Options.DebugName = DebugName;
            Options.EntryPoint = EntryPoint;
            Options.bGenerateReflectionData = false;

            GShaderCompiler->CompilerShaderRaw(FString(Shaders::kVoxelCommon) + Module, Options,
                [&Spirv](FShaderHeader Header) { Spirv = Move(Header.Binaries); });

            GShaderCompiler->Flush();

            if (Spirv.empty())
            {
                LOG_ERROR("Grain: failed to compile '{}'.", DebugName);
            }
            return Spirv;
        }

        TVector<uint32> CompileEntry(const char* EntryPoint, const char* DebugName)
        {
            return CompileFrom(Shaders::kModule, EntryPoint, DebugName);
        }

        // Slang does not always keep the source name, so the pipeline takes whatever OpEntryPoint holds.
        FString EntryPointName(const TVector<uint32>& Spirv)
        {
            constexpr uint32 kOpEntryPoint = 15u;
            constexpr size_t kHeaderWords = 5;

            for (size_t Word = kHeaderWords; Word + 3 < Spirv.size();)
            {
                const uint32 Count = Spirv[Word] >> 16u;
                if (Count == 0)
                {
                    break;
                }

                if ((Spirv[Word] & 0xFFFFu) == kOpEntryPoint)
                {
                    const char* Text = reinterpret_cast<const char*>(&Spirv[Word + 3]);
                    const size_t Limit = (Count - 3) * sizeof(uint32);
                    return FString(Text, strnlen(Text, Limit));
                }

                Word += Count;
            }
            return FString("main");
        }

        RHI::FShaderSource ShaderSource(const TVector<uint32>& Spirv, const char* EntryPoint)
        {
            return RHI::FShaderSource
            {
                .Source     = TSpan<const std::byte>(reinterpret_cast<const std::byte*>(Spirv.data()),
                                                     Spirv.size() * sizeof(uint32)),
                .EntryPoint = EntryPoint,
            };
        }

        RHI::FPipelineH MakePipeline(const TVector<uint32>& Vertex, const TVector<uint32>& Pixel,
                                     const char* PixelEntry, EFormat Format, uint32 Count = 1)
        {
            if (Vertex.empty() || Pixel.empty())
            {
                return {};
            }

            RHI::FColorTarget ColorTargets[4];
            for (uint32 i = 0; i < Count; ++i)
            {
                ColorTargets[i] = RHI::FColorTarget { .Format = Format };
            }

            RHI::FRasterDesc Raster;
            Raster.ColorTargets = TSpan<const RHI::FColorTarget>(ColorTargets, Count);

            return RHI::CreateGraphicsPipeline(ShaderSource(Vertex, "FullscreenVS"),
                                               ShaderSource(Pixel, PixelEntry), Raster);
        }

        // RHI::Utils only opens a single target pass, and the raymarch splits across three.
        void BeginMultiPass(RHI::FCmdListH CL, TSpan<const RHI::FTextureH> Targets,
                            const FUIntVector2& Extent, RHI::FDepthStencilH DepthState)
        {
            RHI::FRenderAttachment Attachments[4];
            for (size_t i = 0; i < Targets.size(); ++i)
            {
                Attachments[i] = RHI::FRenderAttachment
                {
                    .Texture = Targets[i],
                    .LoadOp  = RHI::ELoadOp::Clear,
                    .StoreOp = RHI::EStoreOp::Store,
                    .Color   = { 0.0f, 0.0f, 0.0f, 1.0f },
                };
            }

            const RHI::FRenderPassDesc Pass
            {
                .ColorAttachments = TSpan<const RHI::FRenderAttachment>(Attachments, Targets.size()),
                .RenderArea       = Extent,
            };

            const RHI::FRect Rect { 0, int32(Extent.x), 0, int32(Extent.y) };

            RHI::CmdBeginRenderPass(CL, Pass);
            RHI::CmdSetDepthStencilState(CL, DepthState);
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
            RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CCW);
            RHI::CmdSetViewport(CL, Rect);
            RHI::CmdSetScissor(CL, Rect);
        }

        FVector4 MakeVector4(const FVector3& V, float W)
        {
            return { V.x, V.y, V.z, W };
        }
    }

    bool FRenderer::Initialize(EFormat InSwapchainFormat)
    {
        SwapchainFormat = InSwapchainFormat;
        DepthState = RHI::CreateDepthStencil(RHI::FDepthStencilDesc{});

        return CreatePipelines();
    }

    bool FRenderer::CreatePipelines()
    {
        const TVector<uint32> Vertex     = CompileEntry("FullscreenVS", "Grain.Fullscreen");
        const TVector<uint32> Raymarch   = CompileEntry("RaymarchPS", "Grain.Raymarch");
        const TVector<uint32> Downsample = CompileEntry("DownsamplePS", "Grain.Downsample");
        const TVector<uint32> Upsample   = CompileEntry("UpsamplePS", "Grain.Upsample");
        const TVector<uint32> Temporal   = CompileEntry("TemporalPS", "Grain.Temporal");
        const TVector<uint32> Atrous     = CompileEntry("AtrousPS", "Grain.Atrous");
        const TVector<uint32> Compose    = CompileEntry("ComposePS", "Grain.Compose");
        const TVector<uint32> Composite  = CompileEntry("CompositePS", "Grain.Composite");
        const TVector<uint32> SimStep    = CompileFrom(Shaders::kSimModule, "SimFlowCS", "Grain.SimFlow");
        const TVector<uint32> SimCoarse  = CompileFrom(Shaders::kSimModule, "SimCoarseCS", "Grain.SimCoarse");
        const TVector<uint32> Pick       = CompileFrom(Shaders::kSimModule, "PickCS", "Grain.Pick");
        const TVector<uint32> Destroy    = CompileFrom(Shaders::kSimModule, "DestroyCS", "Grain.Destroy");

        RaymarchPipeline   = MakePipeline(Vertex, Raymarch, "RaymarchPS", kSceneFormat, 3);
        TemporalPipeline   = MakePipeline(Vertex, Temporal, "TemporalPS", kSceneFormat, 2);
        AtrousPipeline     = MakePipeline(Vertex, Atrous, "AtrousPS", kSceneFormat);
        ComposePipeline    = MakePipeline(Vertex, Compose, "ComposePS", kSceneFormat);
        DownsamplePipeline = MakePipeline(Vertex, Downsample, "DownsamplePS", kSceneFormat);
        UpsamplePipeline   = MakePipeline(Vertex, Upsample, "UpsamplePS", kSceneFormat);
        CompositePipeline  = MakePipeline(Vertex, Composite, "CompositePS", SwapchainFormat);

        const FString StepName = EntryPointName(SimStep);
        const FString CoarseName = EntryPointName(SimCoarse);

        if (!SimStep.empty())
        {
            SimStepPipeline = RHI::CreateComputePipeline(ShaderSource(SimStep, StepName.c_str()));
        }
        if (!SimCoarse.empty())
        {
            SimCoarsePipeline = RHI::CreateComputePipeline(ShaderSource(SimCoarse, CoarseName.c_str()));
        }
        if (!Pick.empty())
        {
            PickPipeline = RHI::CreateComputePipeline(ShaderSource(Pick, EntryPointName(Pick).c_str()));
        }
        if (!Destroy.empty())
        {
            DestroyPipeline = RHI::CreateComputePipeline(ShaderSource(Destroy, EntryPointName(Destroy).c_str()));
        }

        PickBuffer = RHI::Malloc(16, RHI::EMemoryType::Default);
        RHI::SetDebugName(PickBuffer.Gpu, "Grain.Pick");

        LOG_INFO("Grain: compute entry points '{}' and '{}'.", StepName, CoarseName);

        return RHI::IsValid(RaymarchPipeline) && RHI::IsValid(DownsamplePipeline)
            && RHI::IsValid(UpsamplePipeline) && RHI::IsValid(CompositePipeline)
            && RHI::IsValid(TemporalPipeline) && RHI::IsValid(AtrousPipeline) && RHI::IsValid(ComposePipeline)
            && RHI::IsValid(SimStepPipeline) && RHI::IsValid(SimCoarsePipeline);
    }

    void FRenderer::ReleaseTargets()
    {
        for (RHI::FManagedTexture* Target : { &RawIndirect, &RawDirect, &RawAlbedo,
                                             &Accum[0], &Accum[1], &Moment[0], &Moment[1],
                                             &Scratch[0], &Scratch[1], &SceneTarget })
        {
            if (Target->IsValid())
            {
                RHI::Textures::Release(*Target);
            }
        }
        BloomChain.Shutdown();
    }

    void FRenderer::EnsureTargets(const FUIntVector2& Extent)
    {
        if (TargetExtent.x == Extent.x && TargetExtent.y == Extent.y)
        {
            return;
        }

        RHI::WaitDeviceIdle();
        ReleaseTargets();

        RHI::FManagedTexture* const Full[] =
        {
            &RawIndirect, &RawDirect, &RawAlbedo, &Accum[0], &Accum[1],
            &Moment[0], &Moment[1], &Scratch[0], &Scratch[1], &SceneTarget,
        };

        for (RHI::FManagedTexture* Target : Full)
        {
            *Target = RHI::Textures::Create(RHI::FTexture2DDesc
            {
                .Width = Extent.x, .Height = Extent.y, .Format = kSceneFormat,
                .bRenderTarget = true, .DebugName = "Grain.Denoise",
            });
        }

        BloomChain.Initialize(Extent, kBloomLevels, kSceneFormat, "Grain.Bloom");

        TargetExtent = Extent;
        bHasHistory = false;
    }

    void FRenderer::StepSim(RHI::FCmdListH CL, const FVoxelSim& Sim)
    {
        if (!Sim.IsValid())
        {
            return;
        }

        const FVector3 Source = Sim.GetSourceVoxels();
        const uint32 Parity = FrameIndex & 1u;

        RHI::CmdBeginMarker(CL, "Grain.Water");
        RHI::CmdSetPipeline(CL, SimStepPipeline);

        // Gravity first, then the two lateral axes, so a cell falls before it tries to spread.
        constexpr uint32 kGroups = uint32(kSimSide) / 4u;
        constexpr uint32 kHalfGroups = kGroups / 2u;

        for (uint32 Axis = 0; Axis < 3; ++Axis)
        {
            FSimArgs Args;
            Args.Grid   = Sim.GetGridAddress();
            Args.Coarse = Sim.GetCoarseAddress();
            Args.Source[0] = uint32(Source.x);
            Args.Source[1] = uint32(Source.y);
            Args.Source[2] = uint32(Source.z);
            Args.Source[3] = Sim.GetSourceMaterial();
            Args.Control[0] = FrameIndex;
            Args.Control[1] = Axis == 0 ? 4u : 0u;
            Args.Control[2] = Axis;
            Args.Control[3] = Parity;

            const uint32 GroupX = Axis == 1 ? kHalfGroups : kGroups;
            const uint32 GroupY = Axis == 0 ? kHalfGroups : kGroups;
            const uint32 GroupZ = Axis == 2 ? kHalfGroups : kGroups;

            RHI::CmdDispatch(CL, RHI::Core::CopyTransient(Args), GroupX, GroupY, GroupZ);
            RHI::Barriers::ComputeToAll(CL);
        }

        FSimArgs CoarseArgs;
        CoarseArgs.Grid   = Sim.GetGridAddress();
        CoarseArgs.Coarse = Sim.GetCoarseAddress();
        CoarseArgs.Source[0] = 0u;
        CoarseArgs.Source[1] = 0u;
        CoarseArgs.Source[2] = 0u;
        CoarseArgs.Source[3] = 0u;
        CoarseArgs.Control[0] = FrameIndex;
        CoarseArgs.Control[1] = 0u;
        CoarseArgs.Control[2] = 0u;
        CoarseArgs.Control[3] = 0u;

        RHI::CmdSetPipeline(CL, SimCoarsePipeline);
        RHI::CmdDispatch(CL, RHI::Core::CopyTransient(CoarseArgs),
            uint32(kSimCoarseSide / 4), uint32(kSimCoarseSide / 4), uint32(kSimCoarseSide / 4));
        RHI::Barriers::ComputeToAll(CL);

        RHI::CmdEndMarker(CL);
    }

    void FRenderer::RunDestroy(RHI::FCmdListH CL, const FVoxelWorld& World, const FVoxelSim& Sim,
                               const FCamera& Camera)
    {
        if (PendingDestroy <= 0.0f)
        {
            return;
        }

        const float Radius = PendingDestroy / kVoxelSize;
        PendingDestroy = 0.0f;

        const FVector3 Position = Camera.GetPosition();
        const FVector3 Forward = Camera.Forward();

        FDestroyArgs Args;
        Args.Nodes     = World.GetNodeAddress();
        Args.Masks     = World.GetMaskAddress();
        Args.Prefix    = World.GetPrefixAddress();
        Args.Children  = World.GetChildAddress();
        Args.SimGrid   = Sim.GetGridAddress();
        Args.SimCoarse = Sim.GetCoarseAddress();
        Args.Pick      = PickBuffer.Gpu;

        Args.Origin    = MakeVector4({ Position.x / kVoxelSize, Position.y / kVoxelSize, Position.z / kVoxelSize }, 0.0f);
        Args.Direction = MakeVector4(Forward, 0.0f);
        Args.SimOrigin = MakeVector4(Sim.GetOriginVoxels(), 0.0f);
        Args.Params    = { Radius, 1400.0f, Sim.IsValid() ? 1.0f : 0.0f, 0.0f };

        const RHI::GPUPtr Block = RHI::Core::CopyTransient(Args);

        RHI::CmdBeginMarker(CL, "Grain.Destroy");

        RHI::CmdSetPipeline(CL, PickPipeline);
        RHI::CmdDispatch(CL, Block, 1, 1, 1);
        RHI::Barriers::ComputeToAll(CL);

        RHI::CmdSetPipeline(CL, DestroyPipeline);
        RHI::CmdDispatch(CL, Block, 2, 2, 2);
        RHI::Barriers::ComputeToAll(CL);

        RHI::CmdEndMarker(CL);

        // The crater invalidates every accumulated pixel that saw the old surface.
        bHasHistory = false;
    }

    void FRenderer::FillViewBasis(FDenoiseArgs& Args, const FUIntVector2& Extent, const FCamera& Camera) const
    {
        const float Aspect = float(Extent.x) / float(Math::Max(Extent.y, 1u));
        const float TanHalfFov = Math::Tan(0.5f * 1.20f);

        const FVector3 Position = Camera.GetPosition();

        Args.CameraPos   = MakeVector4({ Position.x / kVoxelSize, Position.y / kVoxelSize,
                                         Position.z / kVoxelSize }, TanHalfFov);
        Args.CameraFwd   = MakeVector4(Camera.Forward(), Aspect);
        Args.CameraRight = MakeVector4(Camera.Right(), 0.0f);
        Args.CameraUp    = MakeVector4(Camera.Up(), 0.0f);

        Args.PrevPos   = MakeVector4({ PrevPosition.x / kVoxelSize, PrevPosition.y / kVoxelSize,
                                       PrevPosition.z / kVoxelSize }, 0.0f);
        Args.PrevFwd   = MakeVector4(PrevForward, 0.0f);
        Args.PrevRight = MakeVector4(PrevRight, 0.0f);
        Args.PrevUp    = MakeVector4(PrevUp, 0.0f);

        Args.SunDir = MakeVector4(Math::Normalize(FVector3{ 0.80f, 0.29f, -0.52f }), 0.0f);
    }

    void FRenderer::DrawScene(RHI::FCmdListH CL, const FUIntVector2& Extent, const FVoxelWorld& World,
                              const FVoxelSim& Sim, const FCamera& Camera, float RealTime)
    {
        const FVector3 Position = Camera.GetPosition();
        const FVector3 Forward  = Camera.Forward();
        const FVector3 Right    = Camera.Right();
        const FVector3 Up       = Camera.Up();

        const float Aspect = float(Extent.x) / float(Math::Max(Extent.y, 1u));
        const float TanHalfFov = Math::Tan(0.5f * 1.20f);

        const FVector3 SunDirection = Math::Normalize(FVector3{ 0.80f, 0.29f, -0.52f });

        FViewArgs Args;
        Args.Nodes    = World.GetNodeAddress();
        Args.Masks    = World.GetMaskAddress();
        Args.Prefix   = World.GetPrefixAddress();
        Args.Children = World.GetChildAddress();
        Args.Payload  = World.GetPayloadAddress();
        Args.SimGrid   = Sim.GetGridAddress();
        Args.SimCoarse = Sim.GetCoarseAddress();

        const FVector3 SimOrigin = Sim.GetOriginVoxels();
        Args.SimOrigin = MakeVector4(SimOrigin, 0.0f);

        Args.CameraPos   = MakeVector4(Position, TanHalfFov);
        Args.CameraFwd   = MakeVector4(Forward, Aspect);
        Args.CameraRight = MakeVector4(Right, RealTime);
        Args.CameraUp    = MakeVector4(Up, 3.6f);
        Args.SunDir      = MakeVector4(SunDirection, 1.0f);


        // Voxels per pixel at unit distance, which is what drives the traversal's level of detail.
        const float PixelScale = 2.0f * TanHalfFov / float(Math::Max(Extent.y, 1u));
        Args.Params = { float(Extent.x), float(Extent.y), float(FrameIndex), PixelScale };

        Args.DebugMode = DebugMode;
        Args.bSim      = Sim.IsValid() ? 1u : 0u;

        const RHI::FTextureH Targets[] = { RawIndirect.Texture, RawDirect.Texture, RawAlbedo.Texture };

        RHI::CmdBeginMarker(CL, "Grain.Raymarch");
        BeginMultiPass(CL, TSpan<const RHI::FTextureH>(Targets, 3), Extent, DepthState);
        RHI::Utils::DrawFullscreen(CL, RaymarchPipeline, RHI::Core::CopyTransient(Args));
        RHI::Utils::EndScreenPass(CL);
        RHI::CmdEndMarker(CL);
        RHI::Barriers::RasterToRead(CL);
    }

    void FRenderer::Accumulate(RHI::FCmdListH CL, const FUIntVector2& Extent, const FCamera& Camera, bool bMoved)
    {
        const int32 ReadIndex = WriteIndex ^ 1;

        FDenoiseArgs Args;
        FillViewBasis(Args, Extent, Camera);

        Args.IDs[0] = RawIndirect.SampledSlot;
        Args.IDs[1] = Accum[ReadIndex].SampledSlot;
        Args.IDs[2] = Moment[ReadIndex].SampledSlot;

        Args.Extra[0] = (bHasHistory && bTemporal && DebugMode == 0) ? 1u : 0u;

        // A moving camera keeps a shorter history, since reprojection error grows with motion.
        const float MaxHistory = bMoved ? 12.0f : 48.0f;
        Args.Params = { float(Extent.x), float(Extent.y), MaxHistory, 0.02f };

        const RHI::FTextureH Targets[] = { Accum[WriteIndex].Texture, Moment[WriteIndex].Texture };

        RHI::CmdBeginMarker(CL, "Grain.Temporal");
        BeginMultiPass(CL, TSpan<const RHI::FTextureH>(Targets, 2), Extent, DepthState);
        RHI::Utils::DrawFullscreen(CL, TemporalPipeline, RHI::Core::CopyTransient(Args));
        RHI::Utils::EndScreenPass(CL);
        RHI::CmdEndMarker(CL);
        RHI::Barriers::RasterToRead(CL);

        PrevPosition = Camera.GetPosition();
        PrevForward  = Camera.Forward();
        PrevRight    = Camera.Right();
        PrevUp       = Camera.Up();
        bHasHistory  = true;
    }

    void FRenderer::FilterIndirect(RHI::FCmdListH CL, const FUIntVector2& Extent)
    {
        FilterOutput = -1;

        if (!bFilter || DebugMode != 0)
        {
            return;
        }

        RHI::CmdBeginMarker(CL, "Grain.Atrous");

        // Each pass doubles its reach, so a handful of five tap passes covers a wide neighborhood.
        for (int32 Pass = 0; Pass < kAtrousPasses; ++Pass)
        {
            const int32 Output = Pass & 1;

            FDenoiseArgs Args;
            Args.IDs[0] = Pass == 0 ? Accum[WriteIndex].SampledSlot : Scratch[Output ^ 1].SampledSlot;
            Args.IDs[1] = RawIndirect.SampledSlot;
            Args.IDs[2] = RawDirect.SampledSlot;
            Args.Params = { float(Extent.x), float(Extent.y), float(1 << Pass), 0.08f };

            RHI::Utils::BeginScreenPass(CL, { .Target = Scratch[Output].Texture, .Extent = Extent,
                .DepthState = DepthState });
            RHI::Utils::DrawFullscreen(CL, AtrousPipeline, RHI::Core::CopyTransient(Args));
            RHI::Utils::EndScreenPass(CL);
            RHI::Barriers::RasterToRead(CL);

            FilterOutput = Output;
        }

        RHI::CmdEndMarker(CL);
    }

    void FRenderer::Compose(RHI::FCmdListH CL, const FUIntVector2& Extent, const FCamera& Camera)
    {
        FDenoiseArgs Args;
        FillViewBasis(Args, Extent, Camera);

        Args.IDs[0] = RawDirect.SampledSlot;
        Args.IDs[1] = RawAlbedo.SampledSlot;
        Args.IDs[2] = FilterOutput >= 0 ? Scratch[FilterOutput].SampledSlot : Accum[WriteIndex].SampledSlot;
        Args.IDs[3] = RawIndirect.SampledSlot;

        Args.Params = { float(Extent.x), float(Extent.y), 0.0f, 0.0f };

        RHI::CmdBeginMarker(CL, "Grain.Compose");
        RHI::Utils::BeginScreenPass(CL, { .Target = SceneTarget.Texture, .Extent = Extent,
            .DepthState = DepthState });
        RHI::Utils::DrawFullscreen(CL, ComposePipeline, RHI::Core::CopyTransient(Args));
        RHI::Utils::EndScreenPass(CL);
        RHI::CmdEndMarker(CL);
        RHI::Barriers::RasterToRead(CL);
    }

    void FRenderer::DrawBloom(RHI::FCmdListH CL)
    {
        RHI::CmdBeginMarker(CL, "Grain.Bloom");

        for (int32 Level = 0; Level < kBloomLevels; ++Level)
        {
            const uint32 SourceID = Level == 0
                ? SceneTarget.SampledSlot
                : BloomChain.SampledSlot(Level - 1);

            const FVector2 SourceTexel = Level == 0
                ? FVector2 { 1.0f / float(TargetExtent.x), 1.0f / float(TargetExtent.y) }
                : BloomChain.TexelSize(Level - 1);

            const FBloomArgs Args
            {
                .SourceID        = SourceID,
                .bFirstPass      = Level == 0 ? 1u : 0u,
                .SourceTexelSize = SourceTexel,
                .Threshold       = 1.30f,
                .Radius          = 1.0f,
                .Intensity       = 1.0f,
            };

            RHI::Utils::BeginScreenPass(CL, { .Target = BloomChain.Texture(Level),
                .Extent = BloomChain.Extent(Level), .DepthState = DepthState });
            RHI::Utils::DrawFullscreen(CL, DownsamplePipeline, RHI::Core::CopyTransient(Args));
            RHI::Utils::EndScreenPass(CL);
            RHI::Barriers::RasterToRead(CL);
        }

        for (int32 Level = kBloomLevels - 1; Level > 0; --Level)
        {
            const FBloomArgs Args
            {
                .SourceID        = BloomChain.SampledSlot(Level),
                .bFirstPass      = 0u,
                .SourceTexelSize = BloomChain.TexelSize(Level),
                .Threshold       = 0.0f,
                .Radius          = 1.25f,
                .Intensity       = 0.55f,
            };

            RHI::Utils::BeginScreenPass(CL, { .Target = BloomChain.Texture(Level - 1),
                .Extent = BloomChain.Extent(Level - 1), .DepthState = DepthState,
                .LoadOp = RHI::ELoadOp::Load });
            RHI::Utils::DrawFullscreen(CL, UpsamplePipeline, RHI::Core::CopyTransient(Args));
            RHI::Utils::EndScreenPass(CL);
            RHI::Barriers::RasterToRead(CL);
        }

        RHI::CmdEndMarker(CL);
    }

    void FRenderer::DrawComposite(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent)
    {
        const FCompositeArgs Args
        {
            .SceneID        = SceneTarget.SampledSlot,
            .BloomID        = BloomChain.SampledSlot(0),
            .Resolution     = { float(Extent.x), float(Extent.y) },
            .BloomIntensity = 0.11f,
            .Exposure       = 1.25f,
            .Vignette       = 1.0f,
        };

        RHI::CmdBeginMarker(CL, "Grain.Composite");
        RHI::Utils::BeginScreenPass(CL, { .Target = SwapImage, .Extent = Extent, .DepthState = DepthState });
        RHI::Utils::DrawFullscreen(CL, CompositePipeline, RHI::Core::CopyTransient(Args));
        RHI::Utils::EndScreenPass(CL);
        RHI::CmdEndMarker(CL);
    }

    void FRenderer::Render(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent,
                           const FVoxelWorld& World, const FVoxelSim& Sim, const FCamera& Camera,
                           float RealTime, bool bMoved)
    {
        if (Extent.x == 0 || Extent.y == 0 || !BloomChain.IsValid())
        {
            return;
        }

        if (bTimers)
        {
            // The engine profiler disarms collection on every BeginFrame, so it is re-armed here.
            RHI::SetTimestampCollection(true);
            RHI::CmdResetTimestamps(CL, TimerPool, 0, kTimerSlots);
        }

        Mark(CL, 0);
        StepSim(CL, Sim);
        Mark(CL, 1);
        RunDestroy(CL, World, Sim, Camera);
        Mark(CL, 2);
        DrawScene(CL, Extent, World, Sim, Camera, RealTime);
        Mark(CL, 3);
        Accumulate(CL, Extent, Camera, bMoved);
        Mark(CL, 4);
        FilterIndirect(CL, Extent);
        Mark(CL, 5);
        Compose(CL, Extent, Camera);
        Mark(CL, 6);
        DrawBloom(CL);
        Mark(CL, 7);
        DrawComposite(CL, SwapImage, Extent);
        Mark(CL, 8);

        WriteIndex ^= 1;
        ++FrameIndex;
    }

    void FRenderer::EnableGpuTimers()
    {
        if (!RHI::SupportsTimestamps())
        {
            LOG_WARN("Grain: the device reports no timestamp support.");
            return;
        }

        RHI::SetTimestampCollection(true);
        TimerPool = RHI::CreateTimestampPool(kTimerSlots);
        bTimers = RHI::IsValid(TimerPool);
    }

    void FRenderer::Mark(RHI::FCmdListH CL, uint32 Slot)
    {
        if (bTimers)
        {
            RHI::CmdWriteTimestamp(CL, TimerPool, Slot, RHI::EStageFlags::AllCommands);
        }
    }

    void FRenderer::ReportGpuTimers() const
    {
        if (!bTimers)
        {
            return;
        }

        uint64 Ticks[kTimerSlots] = {};
        if (!RHI::ReadTimestamps(TimerPool, 0, kTimerSlots, TSpan<uint64>(Ticks, kTimerSlots)))
        {
            LOG_WARN("Grain: timestamps were not ready.");
            return;
        }

        const double Scale = RHI::GetTimestampPeriodNs() / 1000000.0;

        LOG_INFO("Grain: GPU pass breakdown for the last frame.");
        for (uint32 i = 0; i + 1 < kTimerSlots; ++i)
        {
            const double Milliseconds = double(Ticks[i + 1] - Ticks[i]) * Scale;
            LOG_INFO("  {:<12} {:6.3f} ms", kTimerNames[i], Milliseconds);
        }
        LOG_INFO("  {:<12} {:6.3f} ms", "total", double(Ticks[kTimerSlots - 1] - Ticks[0]) * Scale);
    }

    bool FRenderer::CaptureToFile(const FUIntVector2& Extent, const char* Path)
    {
        // The capture list barriers targets the in flight frame is still writing, so it drains first.
        RHI::WaitDeviceIdle();

        RHI::FTextureDesc Desc;
        Desc.Type      = RHI::ETextureType::Tex2D;
        Desc.Dimension = FUIntVector3(Extent.x, Extent.y, 1);
        // The composite pipeline is built for the swapchain format, so the capture has to match it.
        Desc.Format    = SwapchainFormat;
        Desc.Usage     = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled
                       | RHI::EImageUsageFlags::TransferSrc;

        const RHI::FTextureH Capture = RHI::CreateTexture(Desc);
        if (!RHI::IsValid(Capture))
        {
            return false;
        }

        const uint64 Bytes = uint64(Extent.x) * uint64(Extent.y) * 4u;
        const RHI::FGPUAllocation Readback = RHI::Malloc(Bytes, RHI::EMemoryType::CPURead);

        const RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());

        DrawComposite(CL, Capture, Extent);

        RHI::Barriers::RasterToRead(CL);
        RHI::Barriers::AllToTransfer(CL);

        RHI::FTextureSlice Slice;
        Slice.Extent = FUIntVector3(Extent.x, Extent.y, 1);
        RHI::CmdCopyTextureToMemory(CL, Capture, Slice, Readback.Gpu, Extent.x);
        RHI::Barriers::TransferToAll(CL);

        RHI::Submit(CL);
        RHI::WaitDeviceIdle();

        bool bWrote = false;
        if (const uint8* Pixels = Readback.CpuAs<const uint8>())
        {
            TVector<uint8> Rgba;
            Rgba.resize(size_t(Bytes));
            const bool bSwap = SwapchainFormat == EFormat::BGRA8_UNORM
                            || SwapchainFormat == EFormat::SBGRA8_UNORM;

            for (uint64 i = 0; i < Bytes; i += 4)
            {
                Rgba[i + 0] = bSwap ? Pixels[i + 2] : Pixels[i + 0];
                Rgba[i + 1] = Pixels[i + 1];
                Rgba[i + 2] = bSwap ? Pixels[i + 0] : Pixels[i + 2];
                Rgba[i + 3] = 255;
            }

            bWrote = ImageWrite::WritePngFile(Path, Extent.x, Extent.y, 4, Rgba.data(), Extent.x * 4u);
        }

        RHI::Free(Readback);
        RHI::FreeH(Capture);

        LOG_INFO("Grain: capture '{}' {}.", Path, bWrote ? "written" : "failed");
        return bWrote;
    }

    void FRenderer::Shutdown()
    {
        ReleaseTargets();

        for (RHI::FPipelineH Pipeline : { RaymarchPipeline, DownsamplePipeline, UpsamplePipeline,
                                          CompositePipeline, TemporalPipeline, AtrousPipeline,
                                          ComposePipeline, SimStepPipeline, SimCoarsePipeline,
                                          PickPipeline, DestroyPipeline })
        {
            if (RHI::IsValid(Pipeline))
            {
                RHI::FreeH(Pipeline);
            }
        }

        if (RHI::IsValid(DepthState))
        {
            RHI::FreeH(DepthState);
        }

        if (RHI::IsValid(TimerPool))
        {
            RHI::FreeH(TimerPool);
        }

        if (PickBuffer.Gpu != 0)
        {
            RHI::Free(PickBuffer);
            PickBuffer = {};
        }
    }
}
