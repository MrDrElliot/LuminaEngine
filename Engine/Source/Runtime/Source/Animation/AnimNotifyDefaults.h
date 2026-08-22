#pragma once

#include "AnimNotify.h"
#include "Assets/AssetTypes/Audio/SoundAttenuation.h"
#include "Audio/AudioTypes.h"
#include "Containers/String.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimNotifyDefaults.generated.h"

namespace Lumina
{
    class CAudioStream;
    class CParticleSystem;

    // Nothing includes this header: the picker finds these by reflection, exactly as it finds a game's own.

    REFLECT()
    struct RUNTIME_API SAnimNotify_PlaySound : public SAnimNotify
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Sound")
        TObjectPtr<CAudioStream> Sound;

        /** Socket or bone to play at. Empty plays at the entity's origin. */
        PROPERTY(Editable, Category = "Sound", Picker = "Socket")
        FString Socket;

        /** Extra offset, in the socket's space when one is named, else the entity's. */
        PROPERTY(Editable, Category = "Sound")
        FVector3 Offset = FVector3(0.0f);

        PROPERTY(Editable, Category = "Sound", ClampMin = 0.0f, ClampMax = 4.0f)
        float Volume = 1.0f;

        PROPERTY(Editable, Category = "Sound", ClampMin = 0.01f, ClampMax = 4.0f)
        float Pitch = 1.0f;

        PROPERTY(Editable, Category = "Sound")
        EAudioBus Bus = EAudioBus::SFX;

        /** Off plays a flat 2D one-shot, ignoring the socket position. */
        PROPERTY(Editable, Category = "Sound")
        bool bSpatialized = true;

        PROPERTY(Editable, Category = "Sound")
        SAudioAttenuationSettings Attenuation;

        void Notify(FEntityRegistry& Registry, FEntity Entity) const override;
    };

    REFLECT()
    struct RUNTIME_API SAnimNotify_PlayParticleSystem : public SAnimNotify
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Particle System")
        TObjectPtr<CParticleSystem> ParticleSystem;

        /** Socket or bone to play at. Empty plays at the entity's origin. */
        PROPERTY(Editable, Category = "Particle System", Picker = "Socket")
        FString Socket;

        /** Emitter offset in the spawned effect's local space. */
        PROPERTY(Editable, Category = "Particle System")
        FVector3 Offset = FVector3(0.0f);

        /** Parent the effect to the socket so it follows; off leaves it where it fired. */
        PROPERTY(Editable, Category = "Particle System")
        bool bAttachToSocket = true;

        /** Seconds before the spawned entity is destroyed. 0 leaves it alive for something else to clean up. */
        PROPERTY(Editable, Category = "Particle System", Units = "s", ClampMin = 0.0f)
        float Lifetime = 2.0f;

        void Notify(FEntityRegistry& Registry, FEntity Entity) const override;
    };

    // Debug aid, and it keeps the point picker from ever being empty.
    REFLECT()
    struct RUNTIME_API SAnimNotify_Log : public SAnimNotify
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Log")
        FString Message = "AnimNotify";

        void Notify(FEntityRegistry& Registry, FEntity Entity) const override;
    };

    // The ranged equivalent: logs the window's edges so notify-state timing can be eyeballed.
    REFLECT()
    struct RUNTIME_API SAnimNotifyState_Log : public SAnimNotifyState
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Log")
        FString Message = "AnimNotifyState";

        /** Also log every frame the window is open, which is noisy but shows Alpha advancing. */
        PROPERTY(Editable, Category = "Log")
        bool bLogTick = false;

        void NotifyBegin(FEntityRegistry& Registry, FEntity Entity) const override;
        void NotifyTick(FEntityRegistry& Registry, FEntity Entity, float Alpha) const override;
        void NotifyEnd(FEntityRegistry& Registry, FEntity Entity) const override;
    };
}
