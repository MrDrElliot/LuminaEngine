#pragma once
#include "AudioContext.h"

namespace Lumina
{
    namespace Audio
    {
        NODISCARD RUNTIME_API IAudioContext& Context();
        
        NODISCARD RUNTIME_API bool HasDevice();

        namespace Internal
        {
            /** Publishes the live device and returns the previous one, which the caller then owns.
                Audio::Initialize/Shutdown own this; nothing else should call it. */
            RUNTIME_API IAudioContext* SetContext(IAudioContext* Context);
        }
    }
}
