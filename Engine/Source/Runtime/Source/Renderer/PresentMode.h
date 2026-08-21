#pragma once

#include "Core/Object/ObjectMacros.h"
#include "PresentMode.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EPresentMode : uint8
    {
        // Presents the moment a frame is ready: lowest latency, tears.
        Immediate,

        // Replaces the queued frame instead of waiting: no tearing, renders uncapped.
        Mailbox,

        // Waits for the display refresh: no tearing, caps the frame rate. Always supported.
        FIFO,
    };
}
