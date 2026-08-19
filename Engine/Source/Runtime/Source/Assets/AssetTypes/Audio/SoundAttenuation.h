#pragma once

#include "Audio/AudioTypes.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "SoundAttenuation.generated.h"

namespace Lumina
{
    /** Shared 3D falloff, cone and doppler behavior, so one set of values can drive many sources. */
    REFLECT()
    class RUNTIME_API CSoundAttenuation : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        PROPERTY(Editable, Category = "Attenuation")
        SAudioAttenuation Attenuation;
    };

    /** An attenuation asset reference with an inline override, held by anything that plays a spatialized voice. */
    REFLECT()
    struct RUNTIME_API SAudioAttenuationSettings
    {
        GENERATED_BODY()

        /** Shared values. Empty falls back to the inline overrides. */
        PROPERTY(Editable, Category = "Attenuation")
        TObjectPtr<CSoundAttenuation> AttenuationSettings;

        /** Ignores the asset and uses the inline overrides instead. */
        PROPERTY(Editable, Category = "Attenuation", EditCondition = "AttenuationSettings")
        bool bOverrideAttenuation = false;

        PROPERTY(Editable, Category = "Attenuation", EditCondition = "!AttenuationSettings || bOverrideAttenuation", EditConditionHides)
        SAudioAttenuation Overrides;

        const SAudioAttenuation& Resolve() const
        {
            return (AttenuationSettings != nullptr && !bOverrideAttenuation) ? AttenuationSettings->Attenuation : Overrides;
        }
    };
}
