#pragma once

#include "Containers/Function.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina::Agent
{
    enum class EGameThreadResult : uint8
    {
        // The work ran to completion, whether it took longer than the timeout or not.
        Ran,

        // The wait expired before the work started, and it is now guaranteed never to run.
        TimedOut,
    };

    // Runs work on the game thread so a transport thread can touch the world without racing it.
    class EDITOR_API FGameThreadGate
    {
    public:

        // Returns only once the work finished or can never start, so it may capture what the caller owns.
        NODISCARD static EGameThreadResult Run(TMoveOnlyFunction<void()>&& Work, int32 TimeoutMilliseconds);

        // How long Run waits before giving up on work that has not started.
        NODISCARD static int32 GetDefaultTimeoutMilliseconds();
    };
}
