#include "Core/Application/ApplicationGlobalState.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Core/Windows/Window.h"
#include "Core/Windows/WindowTypes.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/SwapchainTarget.h"
#include "Renderer/ShaderCompiler.h"
#include "TaskSystem/TaskSystem.h"

using namespace Lumina;

namespace
{
    // Vulkan clip space puts +Y down, so the apex is the negative one.
    constexpr const char* kVertexShader = R"SLANG(
        static const float2 kPositions[3] = { float2(0.0, -0.65), float2(0.75, 0.65), float2(-0.75, 0.65) };
        static const float3 kColors[3]    = { float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1) };

        struct FVertexOut
        {
            float4 Position : SV_Position;
            float3 Color    : COLOR;
        };

        [shader("vertex")]
        FVertexOut main(uint VertexID : SV_VertexID)
        {
            FVertexOut Out;
            Out.Position = float4(kPositions[VertexID], 0.5, 1.0);
            Out.Color    = kColors[VertexID];
            return Out;
        }
    )SLANG";

    constexpr const char* kPixelShader = R"SLANG(
        struct FVertexOut
        {
            float4 Position : SV_Position;
            float3 Color    : COLOR;
        };

        [shader("fragment")]
        float4 main(FVertexOut In) : SV_Target
        {
            return float4(In.Color, 1.0);
        }
    )SLANG";

    // One entry point per compile, since the compiler concatenates the SPIR-V of every entry point it finds.
    TVector<uint32> CompileSpirv(const char* Source, const char* DebugName)
    {
        TVector<uint32> Spirv;

        FShaderCompileOptions Options;
        Options.DebugName = DebugName;
        Options.bGenerateReflectionData = false;

        GShaderCompiler->CompilerShaderRaw(FString(Source), Options,
            [&Spirv](FShaderHeader Header) { Spirv = Move(Header.Binaries); });

        // The join. Nothing reads Spirv until the compile task has landed.
        GShaderCompiler->Flush();

        if (Spirv.empty())
        {
            LOG_ERROR("Failed to compile '{}'.", DebugName);
        }
        return Spirv;
    }

    RHI::FShaderSource ShaderSource(const TVector<uint32>& Spirv)
    {
        return RHI::FShaderSource
        {
            .Source     = TSpan<const std::byte>(reinterpret_cast<const std::byte*>(Spirv.data()),
                                                 Spirv.size() * sizeof(uint32)),
            .EntryPoint = "main",
        };
    }

    RHI::FPipelineH CreateTrianglePipeline(EFormat ColorFormat)
    {
        const TVector<uint32> Vertex = CompileSpirv(kVertexShader, "HelloTriangle.Vertex");
        const TVector<uint32> Pixel  = CompileSpirv(kPixelShader, "HelloTriangle.Pixel");
        if (Vertex.empty() || Pixel.empty())
        {
            return {};
        }

        const RHI::FColorTarget ColorTarget { .Format = ColorFormat };
        RHI::FRasterDesc Raster;
        Raster.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);

        return RHI::CreateGraphicsPipeline(ShaderSource(Vertex), ShaderSource(Pixel), Raster);
    }

    // Everything the pipeline leaves dynamic has to be set between the pass and the draw.
    void RecordTriangle(RHI::FCmdListH CL, RHI::FTextureH Target, const FUIntVector2& Extent,
                        RHI::FPipelineH Pipeline, RHI::FDepthStencilH DepthState)
    {
        const RHI::FRenderAttachment Color
        {
            .Texture = Target,
            .LoadOp  = RHI::ELoadOp::Clear,
            .StoreOp = RHI::EStoreOp::Store,
            .Color   = { 0.05f, 0.06f, 0.09f, 1.0f },
        };
        const RHI::FRenderPassDesc Pass
        {
            .ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1),
            .RenderArea       = Extent,
        };
        const RHI::FRect Viewport { 0, (int)Extent.x, 0, (int)Extent.y };

        RHI::CmdBeginRenderPass(CL, Pass);
        RHI::CmdSetDepthStencilState(CL, DepthState);
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
        RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CCW);
        RHI::CmdSetViewport(CL, Viewport);
        RHI::CmdSetScissor(CL, Viewport);
        RHI::CmdSetPipeline(CL, Pipeline);
        RHI::CmdDraw(CL, 0, 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
    }

    void RunFrameLoop(FWindow& Window, RHI::FSwapchainTarget& Target,
                      RHI::FPipelineH Pipeline, RHI::FDepthStencilH DepthState)
    {
        for (uint32 FrameSlot = 0; !Window.ShouldClose(); FrameSlot = (FrameSlot + 1) % RHI::kFramesInFlight)
        {
            // Blocks until the GPU has finished with the slot about to be recorded into.
            RHI::Core::BeginFrame(FrameSlot);

            Window.ProcessMessages();
            Target.Resize(Window.GetExtent());

            const RHI::FTextureH SwapImage = Target.Acquire();
            if (!RHI::IsValid(SwapImage))
            {
                continue;
            }

            const RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
            Target.BarrierToRender(CL);

            RecordTriangle(CL, SwapImage, Target.GetExtent(), Pipeline, DepthState);

            Target.Present(CL);
        }
    }
}

int main(int ArgC, char** ArgV)
{
    Memory::Initialize();

    FApplicationGlobalState GlobalState("HelloTriangle Main");
    Task::Initialize();

    FCommandLine ParsedCommandLine { ArgC, ArgV };
    GCommandLine = &ParsedCommandLine;

    // The Vulkan instance asks GLFW for its surface extensions, and only a window brings GLFW up.
    FWindow Window(FWindowSpecs{ .Title = "Lumina Hello Triangle", .Extent = { 1280, 720 } });

    RHI::CreateDevice(RHI::FDeviceDesc{ .bValidation = !ParsedCommandLine.Has("novalidation") });
    RHI::Core::Initialize();

    FSpirVShaderCompiler ShaderCompiler;
    GShaderCompiler = &ShaderCompiler;

    RHI::FSwapchainTarget Target;
    Target.Initialize(RHI::CreateSurface(Window.GetWindow()), Window.GetExtent());

    const RHI::FPipelineH Pipeline = CreateTrianglePipeline(Target.GetFormat());
    const RHI::FDepthStencilH DepthState = RHI::CreateDepthStencil(RHI::FDepthStencilDesc{});

    const bool bCompiled = RHI::IsValid(Pipeline);
    if (bCompiled)
    {
        RunFrameLoop(Window, Target, Pipeline, DepthState);
    }

    RHI::WaitDeviceIdle();

    Target.Shutdown();

    if (bCompiled)
    {
        RHI::FreeH(Pipeline);
    }
    RHI::FreeH(DepthState);

    GShaderCompiler = nullptr;
    RHI::Core::Shutdown();
    RHI::FreeDevice();

    Task::Shutdown();
    GCommandLine = nullptr;

    return bCompiled ? 0 : 1;
}
