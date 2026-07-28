#include "pch.h"

#include "Core/Console/ConsoleVariable.h"
#include "Log/Log.h"
#include "Renderer/RHI.h"

namespace Lumina
{
    static FAutoConsoleCommand GCrashCPUCommand(
        "crash.cpu",
        "Trigger a CPU access violation to test the crash handler / minidump path. Writes to a null pointer.",
        []()
        {
            LOG_CRITICAL("crash.cpu: triggering deliberate CPU access violation");
            Logging::Flush();

            volatile int* Null = nullptr;
            *Null = 0xDEADBEEF;
        });

    static FAutoConsoleCommand GCrashGPUCommand(
        "crash.gpu",
        "Trigger the device-lost handler to test the GPU crash pipeline (Aftermath dump + panic). Does not actually fault the GPU.",
        []()
        {
            LOG_CRITICAL("crash.gpu: invoking HandleDeviceLost() to exercise GPU crash pipeline");
            Logging::Flush();

            RHI::HandleDeviceLost();
        });
}
