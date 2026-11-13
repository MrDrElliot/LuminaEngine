#include "RHITestHarness.h"

#include "Renderer/RHITexture.h"
#include "Renderer/RHIUpload.h"
#include "Renderer/ShaderCompiler.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Lumina::RHITests
{
    namespace
    {
        struct FRegisteredTest
        {
            const char* Group = nullptr;
            const char* Name  = nullptr;
            FTestFn     Fn    = nullptr;
            bool        bExpectValidationError = false;
        };

        // Function-local so registration from another TU's static init cannot race construction order.
        TVector<FRegisteredTest>& Registry()
        {
            static TVector<FRegisteredTest> Tests;
            return Tests;
        }

        //~ The debug messenger fires on whatever thread the driver reports on, so the sink is guarded.
        FMutex           GValidationMutex;
        TVector<FString> GValidationMessages;
        bool             GCapturing = false;

        void OnValidationMessage(RHI::EValidationSeverity Severity, const char* Message, void*)
        {
            if (Severity != RHI::EValidationSeverity::Error && Severity != RHI::EValidationSeverity::Warning)
            {
                return;
            }

            FScopeLock Lock(GValidationMutex);
            if (GCapturing)
            {
                GValidationMessages.Emplace(Message);
            }
        }

        void BeginCapture()
        {
            FScopeLock Lock(GValidationMutex);
            GValidationMessages.Reset();
            GCapturing = true;
        }

        // Returns what the driver complained about while the test ran, and stops capturing.
        TVector<FString> EndCapture()
        {
            FScopeLock Lock(GValidationMutex);
            GCapturing = false;
            TVector<FString> Out = Move(GValidationMessages);
            GValidationMessages.Reset();
            return Out;
        }

        bool MatchesPattern(const char* Pattern, const char* PatternEnd, const char* Name)
        {
            if (Pattern == PatternEnd)
            {
                return *Name == '\0';
            }
            if (*Pattern == '*')
            {
                return MatchesPattern(Pattern + 1, PatternEnd, Name) || (*Name != '\0' && MatchesPattern(Pattern, PatternEnd, Name + 1));
            }
            return *Name != '\0' && (*Pattern == '?' || *Pattern == *Name) && MatchesPattern(Pattern + 1, PatternEnd, Name + 1);
        }

        // The positive half of a --gtest_filter, colon-separated patterns of '*' and '?'.
        bool MatchesFilter(const char* Filter, const char* Name)
        {
            for (const char* Pattern = Filter;;)
            {
                const char* End = std::strchr(Pattern, ':');
                if (End == nullptr)
                {
                    End = Pattern + std::strlen(Pattern);
                }
                if (MatchesPattern(Pattern, End, Name))
                {
                    return true;
                }
                if (*End == '\0')
                {
                    return false;
                }
                Pattern = End + 1;
            }
        }
    }

    FTestRegistrar::FTestRegistrar(const char* Group, const char* Name, FTestFn Fn, bool bExpectValidationError)
    {
        Registry().Emplace(Group, Name, Fn, bExpectValidationError);
    }

    RHI::FTextureDesc MakeSampledDesc(uint32 Size, EFormat Format, RHI::EImageUsageFlags Extra)
    {
        RHI::FTextureDesc Desc;
        Desc.Type      = RHI::ETextureType::Tex2D;
        Desc.Dimension = FUIntVector3(Size, Size, 1);
        Desc.Format    = Format;
        Desc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst
                       | RHI::EImageUsageFlags::TransferSrc | Extra;
        return Desc;
    }

    bool FTestContext::Check(bool bCondition, const char* Expression, const char* File, int32 Line)
    {
        if (!bCondition)
        {
            Failf("%s(%d): error: CHECK failed: %s", File, Line, Expression);
        }
        return bCondition;
    }

    void FTestContext::Failf(const char* Format, ...)
    {
        char Buffer[1024];

        va_list Args;
        va_start(Args, Format);
        std::vsnprintf(Buffer, sizeof(Buffer), Format, Args);
        va_end(Args);

        Failures.Emplace(Buffer);
    }

    RHI::FGPUAllocation FTestContext::Malloc(uint64 Size, RHI::EMemoryType Type, const char* DebugName)
    {
        const RHI::FGPUAllocation Allocation = RHI::Malloc(Size, RHI::kDefaultAlign, Type);
        if (Allocation.Gpu != 0)
        {
            RHI::SetDebugName(Allocation.Gpu, DebugName);
            ScratchBuffers.Add(Allocation);
        }
        return Allocation;
    }

    RHI::FTextureH FTestContext::CreateTexture(const RHI::FTextureDesc& Desc, const char* DebugName)
    {
        const RHI::FTextureH Texture = RHI::CreateTexture(Desc);
        if (RHI::IsValid(Texture))
        {
            RHI::SetDebugName(Texture, DebugName);
            ScratchTextures.Add(Texture);
        }
        return Texture;
    }

    RHI::FCmdListH FTestContext::OpenCL()
    {
        const RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::GetGlobalHeap());
        return CL;
    }

    void FTestContext::SubmitAndWait(RHI::FCmdListH CL)
    {
        const uint64 Value = RHI::Submit(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{&CL, 1});
        RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Graphics), Value);
    }

    void FTestContext::PumpFrames(uint32 Count)
    {
        for (uint32 i = 0; i < Count; ++i)
        {
            RHI::BeginFrame(FrameIndex);
            FrameIndex = (FrameIndex + 1) % RHI::kFramesInFlight;
        }
    }

    TVector<uint32> FTestContext::TryCompileShader(const char* Source, const char* DebugName, const char* EntryPoint)
    {
        if (Global::GShaderCompiler == nullptr)
        {
            Failf("no shader compiler (GShaderCompiler is null)");
            return {};
        }

        TVector<uint32> Spirv;

        FShaderCompileOptions Options;
        Options.DebugName = DebugName;
        Options.bGenerateReflectionData = false;

        if (EntryPoint != nullptr)
        {
            Options.EntryPoint = EntryPoint;
        }

        // Flush below is the join, so nothing reads Spirv until every pending task has drained.
        Global::GShaderCompiler->CompilerShaderRaw(FString(Source), Options,
            [&Spirv](FShaderHeader Header) { Spirv = Move(Header.Binaries); });

        Global::GShaderCompiler->Flush();

        return Spirv;
    }

    TVector<uint32> FTestContext::CompileShader(const char* Source, const char* DebugName, const char* EntryPoint)
    {
        TVector<uint32> Spirv = TryCompileShader(Source, DebugName, EntryPoint);

        if (Spirv.IsEmpty())
        {
            Failf("shader '%s' failed to compile (see the Slang diagnostics above)", DebugName);
        }
        else if (Spirv[0] != 0x07230203u)
        {
            Failf("shader '%s' produced %zu words with a bad SPIR-V magic (%#x)",
                  DebugName, (size_t)Spirv.Num(), Spirv[0]);
        }

        return Spirv;
    }

    RHI::FPipelineH FTestContext::TrackPipeline(RHI::FPipelineH Pipeline)
    {
        if (RHI::IsValid(Pipeline))
        {
            ScratchPipelines.Add(Pipeline);
        }
        return Pipeline;
    }

    void FTestContext::ReleaseScratch()
    {
        // A test may leave work in flight, so a synchronous teardown provokes destroy-in-use itself.
        for (auto It = ScratchPipelines.rbegin(); It != ScratchPipelines.rend(); ++It)
        {
            RHI::Retire(*It);
        }
        for (auto It = ScratchTextures.rbegin(); It != ScratchTextures.rend(); ++It)
        {
            RHI::Retire(*It);
        }
        for (auto It = ScratchBuffers.rbegin(); It != ScratchBuffers.rend(); ++It)
        {
            RHI::Retire(*It);
        }
        ScratchPipelines.Reset();
        ScratchTextures.Reset();
        ScratchBuffers.Reset();
    }

    void ForEachTest(void (*Visitor)(const char* Group, const char* Name))
    {
        for (const FRegisteredTest& Test : Registry())
        {
            Visitor(Test.Group, Test.Name);
        }
    }

    int32 RunAll(const char* Filter)
    {
        RHI::SetValidationHandler(&OnValidationMessage, nullptr);

        int32 Passed  = 0;
        int32 Failed  = 0;
        int32 Skipped = 0;

        const bool bFiltered = Filter != nullptr && *Filter != '\0';

        // SelfTest provokes real faults, so it runs only when a filter names it outright.
        const bool bSelfTestNamed = bFiltered && std::strstr(Filter, "SelfTest") != nullptr;

        // Unprefixed gtest lines, since LuminaBuild Test and lumi verify read results from them.
        std::printf("[==========] Running RHITests\n");

        for (const FRegisteredTest& Test : Registry())
        {
            char Qualified[256];
            std::snprintf(Qualified, sizeof(Qualified), "%s.%s", Test.Group, Test.Name);

            if ((bFiltered && !MatchesFilter(Filter, Qualified)) || (!bSelfTestNamed && std::strcmp(Test.Group, "SelfTest") == 0))
            {
                ++Skipped;
                continue;
            }

            std::printf("[ RUN      ] %s\n", Qualified);
            std::fflush(stdout);

            FTestContext Ctx;
            BeginCapture();

            Test.Fn(Ctx);

            // Deferred destruction happens in this window, so a lifetime bug lands in THIS test's capture.
            Ctx.ReleaseScratch();
            Ctx.PumpFrames(RHI::kFramesInFlight * 2 + 1);
            RHI::WaitDeviceIdle();

            const TVector<FString> Validation = EndCapture();

            if (Test.bExpectValidationError)
            {
                // The driver complaining IS the pass condition, so silence means the check went dark.
                if (Validation.IsEmpty())
                {
                    Ctx.Failf("expected a validation error, but none was reported "
                              "(GPU-AV off, or the capture path is broken)");
                }
                else
                {
                    std::printf("             caught: %s\n", *Validation[0]);
                }
            }
            else
            {
                for (const FString& Message : Validation)
                {
                    Ctx.Failf("validation: %s", *Message);
                }
            }

            if (Ctx.HasFailures())
            {
                ++Failed;
                for (const FString& Failure : Ctx.GetFailures())
                {
                    std::printf("             %s\n", *Failure);
                }
                std::printf("[  FAILED  ] %s\n", Qualified);
            }
            else
            {
                ++Passed;
                std::printf("[       OK ] %s\n", Qualified);
            }
            std::fflush(stdout);
        }

        RHI::SetValidationHandler(nullptr, nullptr);

        std::printf("[==========] %d tests ran. %d filtered out.\n", Passed + Failed, Skipped);
        std::printf("[  PASSED  ] %d tests.\n", Passed);
        std::fflush(stdout);

        return Failed;
    }
}
