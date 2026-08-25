#pragma once

#include "Audio/AudioTypes.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CSoundBase;
    class FAudioGraphInstance;

    /** A started voice, plus the graph behind it when the sound was a graph rather than a wave. */
    struct FSoundPlayResult
    {
        FAudioHandle Handle;

        /** Null for a wave. Hold it only to push parameters without a handle lookup. */
        TSharedPtr<FAudioGraphInstance> GraphInstance;

        bool IsValid() const { return Handle.IsValid(); }
    };

    namespace Audio
    {
        // One dispatch for every sound kind, so a component, an anim notify and a script all behave alike.
        RUNTIME_API FSoundPlayResult PlaySound(CSoundBase* Sound, const FAudioPlayParams& Params);
    }
}
