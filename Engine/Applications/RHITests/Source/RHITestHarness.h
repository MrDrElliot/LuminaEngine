#pragma once

#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"

namespace Lumina::RHITests
{
    /** Handed to every test body. Owns the per-test failure list and the scratch resources the test
     *  allocates, so a test that fails half way through still tears its GPU objects down. */
    class FTestContext
    {
    public:

        //~ Assertions. All of them record and continue -- a test states several things and we want every
        //  one of them reported, not just the first.

        bool Check(bool bCondition, const char* Expression, const char* File, int32 Line);

        template<typename T, typename U>
        bool CheckEq(const T& Actual, const U& Expected, const char* Expression, const char* File, int32 Line)
        {
            if (Actual == (T)Expected)
            {
                return true;
            }
            Failf("%s == %s  (got %llu, want %llu)  [%s:%d]", Expression, "expected",
                  (unsigned long long)Actual, (unsigned long long)Expected, File, Line);
            return false;
        }

        void Failf(const char* Format, ...);

        //~ Resource helpers. Everything taken through these is released when the test returns, in
        //  reverse order of acquisition, whether it passed or failed.

        RHI::FGPUAllocation Malloc(uint64 Size, RHI::EMemoryType Type, const char* DebugName);
        RHI::FTextureH CreateTexture(const RHI::FTextureDesc& Desc, const char* DebugName);

        /** Command list with the global texture heap already bound. Not auto-submitted: a test that wants
         *  its work in flight rather than completed submits it itself. */
        RHI::FCmdListH OpenCL();

        /** Record -> submit -> wait on this submission's own timeline value. The default for a test: one
         *  command list per step means the GPU has quiesced before the next step records anything. */
        void SubmitAndWait(RHI::FCmdListH CL);

        /** Advance the frame ring N times. Drives the retire queue, the upload flush and command-list
         *  recycling, which is where deferred destruction actually happens. */
        void PumpFrames(uint32 Count);

        /** Compiles self-contained Slang to SPIR-V and blocks until it lands. Records a failure and
         *  returns empty if the compile failed, so the caller can RHI_REQUIRE on emptiness. */
        TVector<uint32> CompileShader(const char* Source, const char* DebugName);

        /** Pipeline handle retired with the test's scratch. Pipelines must never be freed synchronously
         *  while frames are in flight -- RHI::FreeH destroys the VkPipeline immediately. */
        RHI::FPipelineH TrackPipeline(RHI::FPipelineH Pipeline);

        const TVector<FString>& GetFailures() const { return Failures; }
        bool HasFailures() const { return !Failures.empty(); }

        /** Runner-only: retires everything taken through Malloc/CreateTexture. Called after the body
         *  returns, before the frames that let the retire queue reach it. */
        void ReleaseScratch();

    private:

        TVector<FString>         Failures;
        TVector<RHI::FGPUAllocation> ScratchBuffers;
        TVector<RHI::FTextureH>  ScratchTextures;
        TVector<RHI::FPipelineH> ScratchPipelines;
        uint32                   FrameIndex = 0;
    };

    /** SPIR-V words as an FShaderSource. The entry-point name must outlive the pipeline create call --
     *  a string literal always does. */
    inline RHI::FShaderSource ShaderSource(const TVector<uint32>& Spirv, const char* EntryPoint = "main")
    {
        return RHI::FShaderSource
        {
            .Source     = TSpan<const std::byte>(reinterpret_cast<const std::byte*>(Spirv.data()),
                                                 Spirv.size() * sizeof(uint32)),
            .EntryPoint = EntryPoint,
        };
    }

    /** Shared descriptor builders. These live here rather than in each test file because the module is a
     *  unity build: per-file anonymous namespaces are merged into one translation unit, so two files
     *  cannot each define their own `SampledDesc`. */
    RHI::FTextureDesc MakeSampledDesc(uint32 Size, EFormat Format = EFormat::RGBA8_UNORM,
                                      RHI::EImageUsageFlags Extra = RHI::EImageUsageFlags::None);

    using FTestFn = void (*)(FTestContext&);

    struct FTestRegistrar
    {
        FTestRegistrar(const char* Group, const char* Name, FTestFn Fn, bool bExpectValidationError = false);
    };

    /** Runs every registered test in registration order. Returns the number that failed.
     *  TestFilter, when set, selects a single "Group.Name" -- the smallest unit that can be put in front
     *  of a driver, which matters when the thing being bisected takes the device down. */
    int32 RunAll(const char* GroupFilter, const char* TestFilter = nullptr);

    /** Registered names, for --list. */
    void ForEachTest(void (*Visitor)(const char* Group, const char* Name));
}

//~ A test body. Registration happens at static-init time, so declaration order in the file is run order.
#define RHI_TEST(Group, Name)                                                                             \
    static void RHITest_##Group##_##Name(Lumina::RHITests::FTestContext& Ctx);                             \
    static const Lumina::RHITests::FTestRegistrar GRHITestReg_##Group##_##Name(                            \
        #Group, #Name, &RHITest_##Group##_##Name);                                                         \
    static void RHITest_##Group##_##Name(Lumina::RHITests::FTestContext& Ctx)

//~ A NEGATIVE CONTROL: the body deliberately breaks a rule, and the test passes only if the validation
//  layer reported something. Inverted so a harness that has stopped seeing validation output at all
//  fails loudly instead of going quietly green.
#define RHI_TEST_EXPECT_VALIDATION(Group, Name)                                                           \
    static void RHITest_##Group##_##Name(Lumina::RHITests::FTestContext& Ctx);                             \
    static const Lumina::RHITests::FTestRegistrar GRHITestReg_##Group##_##Name(                            \
        #Group, #Name, &RHITest_##Group##_##Name, /*bExpectValidationError*/ true);                        \
    static void RHITest_##Group##_##Name(Lumina::RHITests::FTestContext& Ctx)

#define RHI_CHECK(Expr)          Ctx.Check((Expr), #Expr, __FILE__, __LINE__)
#define RHI_CHECK_EQ(A, B)       Ctx.CheckEq((A), (B), #A, __FILE__, __LINE__)

//~ Stops the test body. For a precondition whose failure would make everything after it meaningless
//  (a null allocation, an unsupported queue) rather than just another reported mismatch.
#define RHI_REQUIRE(Expr)                                                                                 \
    do { if (!Ctx.Check((Expr), #Expr, __FILE__, __LINE__)) { return; } } while (false)
