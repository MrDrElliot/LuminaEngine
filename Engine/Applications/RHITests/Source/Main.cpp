#include "RHITestHarness.h"

#include "Core/Application/ApplicationGlobalState.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/ShaderCompiler.h"
#include "TaskSystem/TaskSystem.h"

#include <cstring>

using namespace Lumina;

DECLARE_MODULE_ALLOCATOR_OVERRIDES();

namespace
{
    void PrintUsage()
    {
        std::printf(
            "RHITests -- headless RHI exerciser.\n"
            "\n"
            "  --list             List the registered tests and exit.\n"
            "  --group=<Name>     Run only one group (Device, Memory, Commands, ...).\n"
            "  --test=<G.Name>    Run exactly one test. Use this to bisect anything that can take the\n"
            "                     device down -- it is the smallest workload the harness can submit.\n"
            "  --novalidation     Bring the device up without the validation layer.\n"
            "  --gpuvalidation    Add GPU-assisted validation (instruments shaders; much slower).\n"
            "\n"
            "Every test records its own command list and submits it on its own, so a validation error\n"
            "or a device loss is attributable to a single RHI call.\n");
    }

    void PrintTest(const char* Group, const char* Name)
    {
        std::printf("  %s.%s\n", Group, Name);
    }

    void LogDeviceSummary()
    {
        const RHI::FGPUDeviceInfo Info = RHI::GetDeviceInfo();
        LOG_INFO("Device : {} ({})", Info.Name, Info.bDiscrete ? "discrete" : "integrated");
        LOG_INFO("API    : {}", Info.APIName);
        LOG_INFO("Queues : async compute {}, async transfer {}",
                 RHI::SupportsAsyncCompute() ? "yes" : "no",
                 RHI::SupportsAsyncTransfer() ? "yes" : "no");
    }
}

int main(int ArgC, char** ArgV)
{
    const char* GroupFilter = nullptr;
    const char* TestFilter  = nullptr;
    bool bValidation        = true;
    bool bGpuValidation     = false;

    for (int i = 1; i < ArgC; ++i)
    {
        if (std::strcmp(ArgV[i], "--help") == 0 || std::strcmp(ArgV[i], "-h") == 0)
        {
            PrintUsage();
            return 0;
        }
        if (std::strcmp(ArgV[i], "--list") == 0)
        {
            RHITests::ForEachTest(&PrintTest);
            return 0;
        }
        if (std::strcmp(ArgV[i], "--novalidation") == 0)
        {
            bValidation = false;
            continue;
        }
        if (std::strcmp(ArgV[i], "--gpuvalidation") == 0)
        {
            bGpuValidation = true;
            continue;
        }
        if (std::strncmp(ArgV[i], "--group=", 8) == 0)
        {
            GroupFilter = ArgV[i] + 8;
            continue;
        }
        if (std::strncmp(ArgV[i], "--test=", 7) == 0)
        {
            TestFilter = ArgV[i] + 7;
            continue;
        }
    }

    Memory::Initialize();
    
    FApplicationGlobalState GlobalState("RHITests Main");
    Task::Initialize();
    
    bValidation = bValidation || bGpuValidation;
    

    RHI::CreateDevice(RHI::FDeviceDesc
    {
        .bValidation    = bValidation,
        .bDebugUtils    = true,
        .bGpuValidation = bGpuValidation,
        .bHeadless      = true,
    });
    RHI::Core::Initialize();
    
    FSpirVShaderCompiler ShaderCompiler;
    GShaderCompiler = &ShaderCompiler;

    LogDeviceSummary();
    if (!bValidation)
    {
        LOG_WARN("Validation layer disabled: tests can only report their own assertion failures.");
    }
    if (bGpuValidation)
    {
        LOG_INFO("GPU-assisted validation ON.");
    }
    LOG_INFO("");

    const int32 FailedCount = RHITests::RunAll(GroupFilter, TestFilter);

    ShaderCompiler.Flush();
    GShaderCompiler = nullptr;

    RHI::Core::Shutdown();
    RHI::FreeDevice();

    Task::Shutdown();

    return FailedCount == 0 ? 0 : 1;
}
