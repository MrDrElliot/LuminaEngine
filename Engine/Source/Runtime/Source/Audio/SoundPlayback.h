#pragma once

#include "Audio/AudioTypes.h"
#include "Containers/Span.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CSoundBase;
    class FAudioGraphInstance;
    struct SAudioGraphParameterValue;

    /** A started voice, plus the graph behind it when the sound was a graph rather than a wave. */
    struct FSoundPlayResult
    {
        SAudioHandle Handle;

        /** Null for a wave. Hold it only to push parameters without a handle lookup. */
        TSharedPtr<FAudioGraphInstance> GraphInstance;

        bool IsValid() const { return Handle.IsValid(); }
    };

    namespace Audio
    {
        // One dispatch for every sound kind, so a component, an anim notify and a script all behave alike.
        // GraphParameters reach a graph before its first block and are ignored for a wave.
        RUNTIME_API FSoundPlayResult PlaySound(CSoundBase* Sound, const SAudioPlayParams& Params,
            TSpan<const SAudioGraphParameterValue> GraphParameters = {});
    }
}
