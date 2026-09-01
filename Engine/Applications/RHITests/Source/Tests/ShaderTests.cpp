#include "RHITestHarness.h"

namespace Lumina::RHITests
{
    namespace
    {
        constexpr const char* kTwoStageSource = R"SLANG(
            struct FVaryings
            {
                float4 Position : SV_Position;
                float3 Color    : COLOR;
            };

            [shader("vertex")]
            FVaryings EntryPointVS(uint VertexID : SV_VertexID)
            {
                FVaryings Out;
                Out.Position = float4(float(VertexID) * 0.25 - 0.5, -0.5, 0.5, 1.0);
                Out.Color    = float3(1.0, 0.5, 0.25);
                return Out;
            }

            [shader("fragment")]
            float4 EntryPointPS(FVaryings In) : SV_Target
            {
                return float4(In.Color, 1.0);
            }
        )SLANG";

        constexpr uint32 kSpirvMagic = 0x07230203u;
    }

    // Concatenating both stages used to produce a blob that was valid SPIR-V for neither.
    RHI_TEST(Shaders, MultipleEntryPointsAreRejectedWithoutASelection)
    {
        const TVector<uint32> Spirv = Ctx.TryCompileShader(kTwoStageSource, "Shaders.Unselected");

        RHI_CHECK(Spirv.empty());
    }

    RHI_TEST(Shaders, EntryPointSelectsOneStage)
    {
        const TVector<uint32> Vertex = Ctx.CompileShader(kTwoStageSource, "Shaders.Vertex", "EntryPointVS");
        const TVector<uint32> Pixel  = Ctx.CompileShader(kTwoStageSource, "Shaders.Pixel", "EntryPointPS");

        RHI_REQUIRE(!Vertex.empty() && !Pixel.empty());

        RHI_CHECK_EQ(Vertex[0], kSpirvMagic);
        RHI_CHECK_EQ(Pixel[0], kSpirvMagic);

        // A shared cache key would hand both stages the same blob.
        RHI_CHECK(Vertex != Pixel);
    }

    RHI_TEST(Shaders, UnknownEntryPointIsRejected)
    {
        const TVector<uint32> Spirv = Ctx.TryCompileShader(kTwoStageSource, "Shaders.Missing", "NoSuchEntryPoint");

        RHI_CHECK(Spirv.empty());
    }

    // A single-stage module still compiles without naming its entry point.
    RHI_TEST(Shaders, SingleEntryPointNeedsNoSelection)
    {
        constexpr const char* Source = R"SLANG(
            [shader("compute")]
            [numthreads(1, 1, 1)]
            void EntryPointCS(uint3 Id : SV_DispatchThreadID) {}
        )SLANG";

        const TVector<uint32> Spirv = Ctx.CompileShader(Source, "Shaders.Lone");

        RHI_REQUIRE(!Spirv.empty());
        RHI_CHECK_EQ(Spirv[0], kSpirvMagic);
    }
}
