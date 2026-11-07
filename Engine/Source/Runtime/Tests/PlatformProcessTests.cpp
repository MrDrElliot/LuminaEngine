#include <gtest/gtest.h>

#include "Core/Threading/Thread.h"
#include "Platform/Process/PlatformProcess.h"
#include "Platform/Time/PlatformTime.h"

#if defined(LE_PLATFORM_WINDOWS)

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaPlatformProcessTests
{
    namespace Platform = Lumina::Platform;
    namespace PlatformTime = Lumina::PlatformTime;
    using Lumina::FStringView;
    using Lumina::FThreadStop;

    // cmd.exe runs ping as its own child, the shape of a batch file driving a build tool, and ping alone would take a minute.
    constexpr const wchar_t* LongRunningParams = L"/c ping -n 60 127.0.0.1";

    // Only catches a hang, since a loaded machine can stretch any tighter bound.
    constexpr double HangMilliseconds = 30000.0;

    TEST(PlatformProcess, RaisingStopEndsTheCapture)
    {
        FThreadStop Stop;
        const PlatformTime::FStopwatch Timer;

        const int ExitCode = Platform::RunProcessAndWaitCapture(L"cmd.exe", LongRunningParams, nullptr,
            [&Stop](FStringView Line)
            {
                if (!Line.IsEmpty())
                {
                    Stop.Request();
                }
            },
            0, &Stop);

        EXPECT_EQ(ExitCode, 1);
        EXPECT_LT(Timer.ElapsedMilliseconds(), HangMilliseconds);
    }

    TEST(PlatformProcess, TimeoutEndsTheCapture)
    {
        const PlatformTime::FStopwatch Timer;

        const int ExitCode = Platform::RunProcessAndWaitCapture(L"cmd.exe", LongRunningParams, nullptr, [](FStringView) {}, 200);

        EXPECT_EQ(ExitCode, Platform::ProcessTimedOutExitCode);
        EXPECT_LT(Timer.ElapsedMilliseconds(), HangMilliseconds);
    }
}

#endif
