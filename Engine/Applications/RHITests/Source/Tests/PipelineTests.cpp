#include "RHITestHarness.h"

#include "Log/Log.h"
#include "Renderer/RHITexture.h"

namespace Lumina::RHITests
{
    namespace
    {
        // Every source here is SELF-CONTAINED, since the harness never mounts a VFS for Slang to include through.
        constexpr const char* kRHIPrelude = R"SLANG(
            struct FRHIRoot { uint64_t Args; };
            [[vk::push_constant]] FRHIRoot gRHI;
            Ptr<T> GetArgs<T>() { return (T*)gRHI.Args; }
        )SLANG";

        FString WithPrelude(const char* Body)
        {
            return FString(kRHIPrelude) + Body;
        }

        RHI::FTextureDesc ColorTargetDesc(uint32 Size)
        {
            RHI::FTextureDesc Desc;
            Desc.Type      = RHI::ETextureType::Tex2D;
            Desc.Dimension = FUIntVector3(Size, Size, 1);
            Desc.Format    = EFormat::RGBA8_UNORM;
            Desc.Usage     = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled
                           | RHI::EImageUsageFlags::TransferSrc | RHI::EImageUsageFlags::TransferDst;
            return Desc;
        }

        // The two draw paths below differ only in what they record between Begin and EndRenderPass.
        template<typename TRecordFn>
        void RenderAndCheckPixel(FTestContext& Ctx, const char* Name, TRecordFn&& Record)
        {
            constexpr uint32 Size = 32;

            const RHI::FTextureH Target = Ctx.CreateTexture(ColorTargetDesc(Size), Name);
            if (!RHI::IsValid(Target))
            {
                Ctx.Failf("%s: color target creation failed", Name);
                return;
            }

            const uint64 Bytes = (uint64)Size * Size * 4;
            const RHI::GPUPtr Readback = Ctx.Malloc(Bytes, RHI::EMemoryType::CPURead, "RHITests.PixelReadback");
            if (Readback == 0)
            {
                Ctx.Failf("%s: readback allocation failed", Name);
                return;
            }

            const RHI::FDepthStencilH DepthState = RHI::CreateDepthStencil(RHI::FDepthStencilDesc{});

            const RHI::FCmdListH CL = Ctx.OpenCL();

            const RHI::FRenderAttachment Color
            {
                .Texture = Target,
                .LoadOp  = RHI::ELoadOp::Clear,
                .StoreOp = RHI::EStoreOp::Store,
                .Color   = { 0.0f, 0.0f, 0.0f, 1.0f },
            };
            const RHI::FRenderPassDesc Pass
            {
                .ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1),
                .RenderArea       = FUIntVector2(Size, Size),
            };

            RHI::CmdBeginRenderPass(CL, Pass);
            RHI::CmdSetDepthStencilState(CL, DepthState);
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
            RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CW);
            RHI::CmdSetViewport(CL, RHI::FRect{ 0, (int)Size, 0, (int)Size });
            RHI::CmdSetScissor(CL, RHI::FRect{ 0, (int)Size, 0, (int)Size });

            Record(CL);

            RHI::CmdEndRenderPass(CL);

            RHI::Barriers::RasterToRead(CL);
            RHI::Barriers::AllToTransfer(CL);

            RHI::FTextureSlice Slice;
            Slice.Extent = FUIntVector3(Size, Size, 1);
            RHI::CmdCopyTextureToMemory(CL, Target, Slice, Readback, Size);
            RHI::Barriers::TransferToAll(CL);

            Ctx.SubmitAndWait(CL);

            // SubmitAndWait has already retired this submission, so the synchronous free is safe here.
            RHI::FreeH(DepthState);

            const auto* Pixels = static_cast<const uint8*>(RHI::ToHost(Readback));
            if (Pixels == nullptr)
            {
                Ctx.Failf("%s: readback is not host visible", Name);
                return;
            }

            // The shaders below all write pure green, which the black clear cannot be confused with.
            Ctx.CheckEq(Pixels[0], 0u,   "R", __FILE__, __LINE__);
            Ctx.CheckEq(Pixels[1], 255u, "G", __FILE__, __LINE__);
            Ctx.CheckEq(Pixels[2], 0u,   "B", __FILE__, __LINE__);
        }
    }

    RHI_TEST(Pipelines, CompileComputeShader)
    {
        const TVector<uint32> Spirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            [shader("compute")]
            [numthreads(1, 1, 1)]
            void main() { }
        )SLANG").c_str(), "RHITests.CompileOnly");

        RHI_REQUIRE(!Spirv.empty());
        RHI_CHECK_EQ(Spirv[0], 0x07230203u);   // SPIR-V magic
    }

    RHI_TEST(Pipelines, ComputeDispatchWritesBuffer)
    {
        constexpr uint32 Count = 256;

        const TVector<uint32> Spirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            struct FArgs { uint* Output; uint Count; };
            FArgs* Pass() { return GetArgs<FArgs>(); }

            [shader("compute")]
            [numthreads(64, 1, 1)]
            void main(uint3 DTid : SV_DispatchThreadID)
            {
                if (DTid.x >= Pass().Count) { return; }
                Pass().Output[DTid.x] = DTid.x * 3 + 1;
            }
        )SLANG").c_str(), "RHITests.ComputeWrite");
        RHI_REQUIRE(!Spirv.empty());

        const RHI::FPipelineH Pipeline = Ctx.TrackPipeline(RHI::CreateComputePipeline(ShaderSource(Spirv)));
        RHI_REQUIRE(RHI::IsValid(Pipeline));

        const uint64 Bytes = Count * sizeof(uint32);
        const RHI::GPUPtr Output   = Ctx.Malloc(Bytes, RHI::EMemoryType::GPUOnly, "RHITests.ComputeOutput");
        const RHI::GPUPtr Readback = Ctx.Malloc(Bytes, RHI::EMemoryType::CPURead, "RHITests.ComputeReadback");
        RHI_REQUIRE(Output != 0 && Readback != 0);

        struct FArgs { RHI::GPUPtr Output; uint32 Count; uint32 _Pad; };
        const RHI::GPUPtr Args = RHI::Core::CopyTransient(FArgs{ Output, Count, 0 });

        const RHI::FCmdListH CL = Ctx.OpenCL();
        RHI::CmdSetPipeline(CL, Pipeline);
        RHI::CmdDispatch(CL, Args, Count / 64, 1, 1);
        RHI::Barriers::ComputeToAll(CL);
        RHI::CmdMemcpy(CL, Readback, Output, Bytes);
        RHI::Barriers::TransferToAll(CL);
        Ctx.SubmitAndWait(CL);

        const auto* Words = static_cast<const uint32*>(RHI::ToHost(Readback));
        RHI_REQUIRE(Words != nullptr);
        RHI_CHECK_EQ(Words[0], 1u);
        RHI_CHECK_EQ(Words[1], 4u);
        RHI_CHECK_EQ(Words[Count - 1], (Count - 1) * 3 + 1);
    }

    // The argument buffer is produced by one dispatch and consumed by the next with no host round trip.
    RHI_TEST(Pipelines, ComputeDispatchIndirect)
    {
        const TVector<uint32> Spirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            struct FArgs { uint* Output; };
            FArgs* Pass() { return GetArgs<FArgs>(); }

            [shader("compute")]
            [numthreads(1, 1, 1)]
            void main(uint3 Gid : SV_GroupID)
            {
                Pass().Output[Gid.x] = Gid.x + 100;
            }
        )SLANG").c_str(), "RHITests.ComputeIndirect");
        RHI_REQUIRE(!Spirv.empty());

        const RHI::FPipelineH Pipeline = Ctx.TrackPipeline(RHI::CreateComputePipeline(ShaderSource(Spirv)));
        RHI_REQUIRE(RHI::IsValid(Pipeline));

        constexpr uint32 Groups = 8;
        const RHI::GPUPtr IndirectArgs = Ctx.Malloc(sizeof(RHI::FDispatchIndirectArguments),
                                                    RHI::EMemoryType::GPUOnly, "RHITests.IndirectArgs");
        const RHI::GPUPtr Output   = Ctx.Malloc(Groups * sizeof(uint32), RHI::EMemoryType::GPUOnly, "RHITests.IndirectOut");
        const RHI::GPUPtr Readback = Ctx.Malloc(Groups * sizeof(uint32), RHI::EMemoryType::CPURead, "RHITests.IndirectReadback");
        RHI_REQUIRE(IndirectArgs != 0 && Output != 0 && Readback != 0);

        const RHI::FDispatchIndirectArguments Dispatch{ Groups, 1, 1 };

        struct FArgs { RHI::GPUPtr Output; };
        const RHI::GPUPtr Args = RHI::Core::CopyTransient(FArgs{ Output });

        const RHI::FCmdListH CL = Ctx.OpenCL();
        RHI::CmdWriteMemory(CL, IndirectArgs, &Dispatch, sizeof(Dispatch));
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::IndirectArguments);

        RHI::CmdSetPipeline(CL, Pipeline);
        RHI::CmdDispatchIndirect(CL, Args, IndirectArgs, 0);
        RHI::Barriers::ComputeToAll(CL);

        RHI::CmdMemcpy(CL, Readback, Output, Groups * sizeof(uint32));
        RHI::Barriers::TransferToAll(CL);
        Ctx.SubmitAndWait(CL);

        const auto* Words = static_cast<const uint32*>(RHI::ToHost(Readback));
        RHI_REQUIRE(Words != nullptr);
        RHI_CHECK_EQ(Words[0], 100u);
        RHI_CHECK_EQ(Words[Groups - 1], 100u + Groups - 1);
    }

    // Writes through the bindless storage heap, the half GPU-AV's descriptor checks actually look at.
    RHI_TEST(Pipelines, ComputeWritesBindlessStorageTexture)
    {
        constexpr uint32 Size = 8;

        const TVector<uint32> Spirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            [[vk::binding(2, 0)]] RWTexture2D<float4> gRWTextures2D[];

            struct FArgs { uint OutUAV; uint Size; };
            FArgs* Pass() { return GetArgs<FArgs>(); }

            [shader("compute")]
            [numthreads(8, 8, 1)]
            void main(uint3 DTid : SV_DispatchThreadID)
            {
                if (DTid.x >= Pass().Size || DTid.y >= Pass().Size) { return; }
                gRWTextures2D[Pass().OutUAV][DTid.xy] = float4(0.0, 1.0, 0.0, 1.0);
            }
        )SLANG").c_str(), "RHITests.ComputeStorageImage");
        RHI_REQUIRE(!Spirv.empty());

        const RHI::FPipelineH Pipeline = Ctx.TrackPipeline(RHI::CreateComputePipeline(ShaderSource(Spirv)));
        RHI_REQUIRE(RHI::IsValid(Pipeline));

        RHI::FManagedTexture Managed = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width    = Size,
            .Height   = Size,
            .Format   = EFormat::RGBA8_UNORM,
            .bStorage = true,
            .DebugName = "RHITests.StorageTarget",
        });
        RHI_REQUIRE(Managed.IsValid());

        const uint32 UAV = RHI::Textures::StorageSlot(Managed, 0);
        RHI_REQUIRE(UAV != RHI::kInvalidHeapSlot);

        const uint64 Bytes = (uint64)Size * Size * 4;
        const RHI::GPUPtr Readback = Ctx.Malloc(Bytes, RHI::EMemoryType::CPURead, "RHITests.StorageReadback");
        RHI_REQUIRE(Readback != 0);

        struct FArgs { uint32 OutUAV; uint32 Size; };
        const RHI::GPUPtr Args = RHI::Core::CopyTransient(FArgs{ UAV, Size });

        const RHI::FCmdListH CL = Ctx.OpenCL();
        RHI::CmdSetPipeline(CL, Pipeline);
        RHI::CmdDispatch(CL, Args, 1, 1, 1);
        RHI::Barriers::ComputeToAll(CL);

        RHI::FTextureSlice Slice;
        Slice.Extent = FUIntVector3(Size, Size, 1);
        RHI::CmdCopyTextureToMemory(CL, Managed.Texture, Slice, Readback, Size);
        RHI::Barriers::TransferToAll(CL);
        Ctx.SubmitAndWait(CL);

        const auto* Pixels = static_cast<const uint8*>(RHI::ToHost(Readback));
        RHI_REQUIRE(Pixels != nullptr);
        RHI_CHECK_EQ(Pixels[0], 0u);
        RHI_CHECK_EQ(Pixels[1], 255u);
        RHI_CHECK_EQ(Pixels[3], 255u);

        RHI::Textures::Release(Managed);
    }

    // Two pipelines from ONE SPIR-V module, so a silently ignored spec constant leaves them identical.
    RHI_TEST(Pipelines, SpecializationConstantChangesResult)
    {
        const TVector<uint32> Spirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            [vk::constant_id(0)] const uint SPEC_ADDEND = 7;

            struct FArgs { uint* Output; };
            FArgs* Pass() { return GetArgs<FArgs>(); }

            [shader("compute")]
            [numthreads(1, 1, 1)]
            void main() { Pass().Output[0] = SPEC_ADDEND; }
        )SLANG").c_str(), "RHITests.SpecConstant");
        RHI_REQUIRE(!Spirv.empty());

        const RHI::GPUPtr Output   = Ctx.Malloc(sizeof(uint32), RHI::EMemoryType::GPUOnly, "RHITests.SpecOutput");
        const RHI::GPUPtr Readback = Ctx.Malloc(sizeof(uint32), RHI::EMemoryType::CPURead, "RHITests.SpecReadback");
        RHI_REQUIRE(Output != 0 && Readback != 0);

        struct FArgs { RHI::GPUPtr Output; };
        const RHI::GPUPtr Args = RHI::Core::CopyTransient(FArgs{ Output });

        auto RunWith = [&](uint32 Value) -> uint32
        {
            RHI::FSpecializationConstant Constant{};
            Constant.ConstantID = 0;
            Constant.AsInt      = Value;
            Constant.Type       = RHI::ESpecializationConstantType::UInt32;

            const RHI::FPipelineH Pipeline = Ctx.TrackPipeline(RHI::CreateComputePipeline(
                ShaderSource(Spirv), TSpan<const RHI::FSpecializationConstant>(&Constant, 1)));
            if (!RHI::IsValid(Pipeline))
            {
                Ctx.Failf("specialized pipeline creation failed for value %u", Value);
                return ~0u;
            }

            const RHI::FCmdListH CL = Ctx.OpenCL();
            RHI::CmdSetPipeline(CL, Pipeline);
            RHI::CmdDispatch(CL, Args, 1, 1, 1);
            RHI::Barriers::ComputeToAll(CL);
            RHI::CmdMemcpy(CL, Readback, Output, sizeof(uint32));
            RHI::Barriers::TransferToAll(CL);
            Ctx.SubmitAndWait(CL);

            const auto* Word = static_cast<const uint32*>(RHI::ToHost(Readback));
            return Word != nullptr ? *Word : ~0u;
        };

        RHI_CHECK_EQ(RunWith(11u), 11u);
        RHI_CHECK_EQ(RunWith(42u), 42u);
    }

    RHI_TEST(Pipelines, GraphicsFullscreenTriangle)
    {
        const TVector<uint32> VSSpirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            [shader("vertex")]
            float4 main(uint VertexID : SV_VertexID) : SV_Position
            {
                float2 UV = float2((VertexID << 1) & 2, VertexID & 2);
                return float4(UV * 2.0 - 1.0, 0.5, 1.0);
            }
        )SLANG").c_str(), "RHITests.FullscreenVS");

        const TVector<uint32> PSSpirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            [shader("fragment")]
            float4 main() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }
        )SLANG").c_str(), "RHITests.FullscreenPS");

        RHI_REQUIRE(!VSSpirv.empty() && !PSSpirv.empty());

        const RHI::FColorTarget ColorTarget{ .Format = EFormat::RGBA8_UNORM };
        RHI::FRasterDesc Raster;
        Raster.Topology     = RHI::ETopology::TriangleList;
        Raster.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);

        const RHI::FPipelineH Pipeline = Ctx.TrackPipeline(
            RHI::CreateGraphicsPipeline(ShaderSource(VSSpirv), ShaderSource(PSSpirv), Raster));
        RHI_REQUIRE(RHI::IsValid(Pipeline));

        RenderAndCheckPixel(Ctx, "RHITests.GraphicsTarget", [&](RHI::FCmdListH CL)
        {
            RHI::CmdSetPipeline(CL, Pipeline);
            RHI::CmdDraw(CL, 0, 3, 1, 0, 0);
        });
    }

    RHI_TEST(Pipelines, MeshShaderDraw)
    {
        const TVector<uint32> MSSpirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            struct FVertexOut { float4 Position : SV_Position; };

            [shader("mesh")]
            [outputtopology("triangle")]
            [numthreads(1, 1, 1)]
            void main(out vertices FVertexOut Verts[3], out indices uint3 Tris[1])
            {
                SetMeshOutputCounts(3, 1);
                Verts[0].Position = float4(-1.0, -1.0, 0.5, 1.0);
                Verts[1].Position = float4( 3.0, -1.0, 0.5, 1.0);
                Verts[2].Position = float4(-1.0,  3.0, 0.5, 1.0);
                Tris[0] = uint3(0, 1, 2);
            }
        )SLANG").c_str(), "RHITests.MeshMS");

        const TVector<uint32> PSSpirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            [shader("fragment")]
            float4 main() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }
        )SLANG").c_str(), "RHITests.MeshPS");

        RHI_REQUIRE(!MSSpirv.empty() && !PSSpirv.empty());

        const RHI::FColorTarget ColorTarget{ .Format = EFormat::RGBA8_UNORM };
        RHI::FRasterDesc Raster;
        Raster.Topology     = RHI::ETopology::TriangleList;
        Raster.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);

        // Task-less, exactly as the engine builds meshlet pipelines once culling has run as compute.
        const RHI::FPipelineH Pipeline = Ctx.TrackPipeline(RHI::CreateMeshShaderPipeline(
            RHI::FShaderSource{}, ShaderSource(MSSpirv), ShaderSource(PSSpirv), Raster));
        RHI_REQUIRE(RHI::IsValid(Pipeline));

        RenderAndCheckPixel(Ctx, "RHITests.MeshTarget", [&](RHI::FCmdListH CL)
        {
            RHI::CmdSetPipeline(CL, Pipeline);
            RHI::CmdDrawMeshTasks(CL, 0, 1, 1, 1);
        });
    }

    RHI_TEST(Pipelines, MeshShaderDrawIndirect)
    {
        const TVector<uint32> MSSpirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            struct FVertexOut { float4 Position : SV_Position; };

            [shader("mesh")]
            [outputtopology("triangle")]
            [numthreads(1, 1, 1)]
            void main(out vertices FVertexOut Verts[3], out indices uint3 Tris[1])
            {
                SetMeshOutputCounts(3, 1);
                Verts[0].Position = float4(-1.0, -1.0, 0.5, 1.0);
                Verts[1].Position = float4( 3.0, -1.0, 0.5, 1.0);
                Verts[2].Position = float4(-1.0,  3.0, 0.5, 1.0);
                Tris[0] = uint3(0, 1, 2);
            }
        )SLANG").c_str(), "RHITests.MeshIndirectMS");

        const TVector<uint32> PSSpirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            [shader("fragment")]
            float4 main() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }
        )SLANG").c_str(), "RHITests.MeshIndirectPS");

        RHI_REQUIRE(!MSSpirv.empty() && !PSSpirv.empty());

        const RHI::FColorTarget ColorTarget{ .Format = EFormat::RGBA8_UNORM };
        RHI::FRasterDesc Raster;
        Raster.Topology     = RHI::ETopology::TriangleList;
        Raster.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);

        const RHI::FPipelineH Pipeline = Ctx.TrackPipeline(RHI::CreateMeshShaderPipeline(
            RHI::FShaderSource{}, ShaderSource(MSSpirv), ShaderSource(PSSpirv), Raster));
        RHI_REQUIRE(RHI::IsValid(Pipeline));

        const RHI::GPUPtr IndirectArgs = Ctx.Malloc(sizeof(RHI::FDrawMeshTasksIndirectArguments),
                                                    RHI::EMemoryType::GPUOnly, "RHITests.MeshIndirectArgs");
        RHI_REQUIRE(IndirectArgs != 0);

        const RHI::FDrawMeshTasksIndirectArguments DrawArgs{ 1, 1, 1 };

        // A transfer write is illegal inside a render pass, so this has to complete before one opens.
        const RHI::FCmdListH Stage = Ctx.OpenCL();
        RHI::CmdWriteMemory(Stage, IndirectArgs, &DrawArgs, sizeof(DrawArgs));
        RHI::CmdBarrier(Stage, RHI::EStageFlags::Transfer, RHI::EStageFlags::IndirectArguments);
        Ctx.SubmitAndWait(Stage);

        RenderAndCheckPixel(Ctx, "RHITests.MeshIndirectTarget", [&](RHI::FCmdListH CL)
        {
            RHI::CmdSetPipeline(CL, Pipeline);
            RHI::CmdDrawMeshTasksIndirect(CL, 0, IndirectArgs, 0, 1, sizeof(RHI::FDrawMeshTasksIndirectArguments));
        });
    }

    RHI_TEST(Pipelines, PipelineStatistics)
    {
        const TVector<uint32> Spirv = Ctx.CompileShader(WithPrelude(R"SLANG(
            struct FArgs { uint* Output; };
            FArgs* Pass() { return GetArgs<FArgs>(); }

            [shader("compute")]
            [numthreads(64, 1, 1)]
            void main(uint3 DTid : SV_DispatchThreadID)
            {
                float Accum = 0.0;
                for (uint i = 0; i < 16; ++i) { Accum += sin((float)(DTid.x + i)); }
                Pass().Output[DTid.x] = (uint)(Accum * 1000.0);
            }
        )SLANG").c_str(), "RHITests.Stats");
        RHI_REQUIRE(!Spirv.empty());

        const RHI::FPipelineH Pipeline = Ctx.TrackPipeline(RHI::CreateComputePipeline(ShaderSource(Spirv)));
        RHI_REQUIRE(RHI::IsValid(Pipeline));

        // Statistics capture is opt-in at device creation, so absent it this must report cleanly.
        TVector<RHI::FPipelineStat> Stats;
        if (!RHI::GetPipelineStatistics(Pipeline, Stats))
        {
            LOG_INFO("             (pipeline statistics unavailable on this device/config)");
            return;
        }

        RHI_CHECK(!Stats.empty());
        for (const RHI::FPipelineStat& Stat : Stats)
        {
            RHI_CHECK(!Stat.Name.empty());
        }
    }
}
