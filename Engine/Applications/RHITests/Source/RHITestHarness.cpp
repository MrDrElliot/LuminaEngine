#include "RHITestHarness.h"

#include "Log/Log.h"
#include "Renderer/RHITexture.h"
#include "Renderer/RHIUpload.h"
#include "Renderer/ShaderCompiler.h"

#include <cstdarg>
#include <cstdio>
#include "Containers/StringFormat.h"

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
                GValidationMessages.push_back(FString(Message));
            }
        }

        void BeginCapture()
        {
            FScopeLock Lock(GValidationMutex);
            GValidationMessages.clear();
            GCapturing = true;
        }

        // Returns what the driver complained about while the test ran, and stops capturing.
        TVector<FString> EndCapture()
        {
            FScopeLock Lock(GValidationMutex);
            GCapturing = false;
            TVector<FString> Out = Move(GValidationMessages);
            GValidationMessages.clear();
            return Out;
        }
    }

    FTestRegistrar::FTestRegistrar(const char* Group, const char* Name, FTestFn Fn, bool bExpectValidationError)
    {
        Registry().push_back(FRegisteredTest{ Group, Name, Fn, bExpectValidationError });
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
            Failf("CHECK failed: %s  [%s:%d]", Expression, File, Line);
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

        Failures.push_back(FString(Buffer));
    }

    RHI::GPUPtr FTestContext::Malloc(uint64 Size, RHI::EMemoryType Type, const char* DebugName)
    {
        const RHI::GPUPtr Ptr = RHI::Malloc(Size, RHI::kDefaultAlign, Type);
        if (Ptr != 0)
        {
            RHI::SetDebugName(Ptr, DebugName);
            ScratchBuffers.push_back(Ptr);
        }
        return Ptr;
    }

    RHI::FTextureH FTestContext::CreateTexture(const RHI::FTextureDesc& Desc, const char* DebugName)
    {
        const RHI::FTextureH Texture = RHI::CreateTexture(Desc);
        if (RHI::IsValid(Texture))
        {
            RHI::SetDebugName(Texture, DebugName);
            ScratchTextures.push_back(Texture);
        }
        return Texture;
    }

    RHI::FCmdListH FTestContext::OpenCL()
    {
        const RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
        return CL;
    }

    void FTestContext::SubmitAndWait(RHI::FCmdListH CL)
    {
        RHI::SubmitAndWait(CL);
        RHI::ResetCommandList(CL);
    }

    void FTestContext::PumpFrames(uint32 Count)
    {
        for (uint32 i = 0; i < Count; ++i)
        {
            RHI::Core::BeginFrame(FrameIndex);
            FrameIndex = (FrameIndex + 1) % RHI::kFramesInFlight;
        }
    }

    TVector<uint32> FTestContext::CompileShader(const char* Source, const char* DebugName)
    {
        if (GShaderCompiler == nullptr)
        {
            Failf("no shader compiler (GShaderCompiler is null)");
            return {};
        }

        TVector<uint32> Spirv;

        FShaderCompileOptions Options;
        Options.DebugName = DebugName;
        Options.bGenerateReflectionData = false;

        // Flush below is the join, so nothing reads Spirv until every pending task has drained.
        GShaderCompiler->CompilerShaderRaw(FString(Source), Options,
            [&Spirv](FShaderHeader Header) { Spirv = Move(Header.Binaries); });

        GShaderCompiler->Flush();

        if (Spirv.empty())
        {
            Failf("shader '%s' failed to compile (see the Slang diagnostics above)", DebugName);
        }
        else if (Spirv[0] != 0x07230203u)
        {
            Failf("shader '%s' produced %zu words with a bad SPIR-V magic (%#x)",
                  DebugName, (size_t)Spirv.size(), Spirv[0]);
        }

        return Spirv;
    }

    RHI::FPipelineH FTestContext::TrackPipeline(RHI::FPipelineH Pipeline)
    {
        if (RHI::IsValid(Pipeline))
        {
            ScratchPipelines.push_back(Pipeline);
        }
        return Pipeline;
    }

    void FTestContext::ReleaseScratch()
    {
        // A test may leave work in flight, so a synchronous teardown provokes destroy-in-use itself.
        for (auto It = ScratchPipelines.rbegin(); It != ScratchPipelines.rend(); ++It)
        {
            RHI::Core::Retire(*It);
        }
        for (auto It = ScratchTextures.rbegin(); It != ScratchTextures.rend(); ++It)
        {
            RHI::Core::Retire(*It);
        }
        for (auto It = ScratchBuffers.rbegin(); It != ScratchBuffers.rend(); ++It)
        {
            RHI::Core::Retire(*It);
        }
        ScratchPipelines.clear();
        ScratchTextures.clear();
        ScratchBuffers.clear();
    }

    void ForEachTest(void (*Visitor)(const char* Group, const char* Name))
    {
        for (const FRegisteredTest& Test : Registry())
        {
            Visitor(Test.Group, Test.Name);
        }
    }

    int32 RunAll(const char* GroupFilter, const char* TestFilter)
    {
        RHI::SetValidationHandler(&OnValidationMessage, nullptr);

        int32 Passed  = 0;
        int32 Failed  = 0;
        int32 Skipped = 0;

        const bool bGroupFiltered = GroupFilter != nullptr && *GroupFilter != '\0';
        const bool bTestFiltered  = TestFilter  != nullptr && *TestFilter  != '\0';
        const bool bFiltered      = bGroupFiltered || bTestFiltered;

        for (const FRegisteredTest& Test : Registry())
        {
            if (bGroupFiltered && strcmp(GroupFilter, Test.Group) != 0)
            {
                ++Skipped;
                continue;
            }

            if (bTestFiltered)
            {
                char Qualified[256];
                std::snprintf(Qualified, sizeof(Qualified), "%s.%s", Test.Group, Test.Name);
                if (strcmp(TestFilter, Qualified) != 0)
                {
                    ++Skipped;
                    continue;
                }
            }

            // SelfTest provokes real faults, so it runs only when the group is named outright.
            if (!bFiltered && strcmp(Test.Group, "SelfTest") == 0)
            {
                ++Skipped;
                continue;
            }

            LOG_INFO("[ RUN      ] {}.{}", Test.Group, Test.Name);

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
                if (Validation.empty())
                {
                    Ctx.Failf("expected a validation error, but none was reported "
                              "(GPU-AV off, or the capture path is broken)");
                }
                else
                {
                    LOG_INFO("             caught: {}", Validation.front());
                }
            }
            else
            {
                for (const FString& Message : Validation)
                {
                    Ctx.Failf("validation: %s", Message.c_str());
                }
            }

            if (Ctx.HasFailures())
            {
                ++Failed;
                for (const FString& Failure : Ctx.GetFailures())
                {
                    LOG_ERROR("             {}", Failure);
                }
                LOG_ERROR("[   FAILED ] {}.{}", Test.Group, Test.Name);
            }
            else
            {
                ++Passed;
                LOG_INFO("[       OK ] {}.{}", Test.Group, Test.Name);
            }
        }

        RHI::SetValidationHandler(nullptr, nullptr);

        LOG_INFO("");
        LOG_INFO("[==========] {} passed, {} failed{}", Passed, Failed,
                 Skipped > 0 ? FString(" (" + Format("{}", Skipped) + " filtered out)") : FString());

        return Failed;
    }
}
