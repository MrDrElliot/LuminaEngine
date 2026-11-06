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
            // Swaps in the live device and hands the previous one back to the caller, only Audio::Initialize and Shutdown call it.
            RUNTIME_API IAudioContext* SetContext(IAudioContext* Context);
        }
    }
}
