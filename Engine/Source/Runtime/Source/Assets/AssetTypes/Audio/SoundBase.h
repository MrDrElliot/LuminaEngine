#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "SoundBase.generated.h"

namespace Lumina
{
    /** Anything a sound source can play, so one component slot accepts a wave or a graph alike. */
    REFLECT()
    class RUNTIME_API CSoundBase : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        /** False for a sound that cannot produce audio yet, e.g. a graph that has never compiled. */
        virtual bool IsPlayable() const { return false; }

        /** Seconds, or 0 for a sound with no fixed length. */
        virtual float GetDuration() const { return 0.0f; }
    };
}
