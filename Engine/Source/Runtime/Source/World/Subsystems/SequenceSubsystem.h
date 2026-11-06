#pragma once

#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "SequenceSubsystem.generated.h"

namespace Lumina
{
    class CSequence;
    struct SSequenceFiredEvent;

    // Sequence playback and the events sequences fire.
    REFLECT()
    class RUNTIME_API CSequenceSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        // Spawns a player entity for Sequence and starts it, returning the player so it can be paused, sought or stopped.
        FUNCTION()
        ECS::FEntity PlaySequence(CSequence* Sequence, bool bLoop = false, float PlayRate = 1.0f);

        // Stops the player and destroys its entity, putting back whatever the sequence displaced.
        FUNCTION()
        void StopSequence(ECS::FEntity Player);

        FUNCTION()
        bool IsSequencePlaying(ECS::FEntity Player);

        // Every event any sequence in the world fired since the last call, oldest first.
        void ConsumeEvents(TVector<SSequenceFiredEvent>& OutEvents);

        // Steps to the next event any sequence fired, false once there are none, then the getters below describe it.
        FUNCTION()
        bool NextEvent();

        FUNCTION()
        FString GetEventName();

        FUNCTION()
        FString GetEventPayload();

        FUNCTION()
        ECS::FEntity GetEventTarget();

        FUNCTION()
        float GetEventTime();

        // Whether any sequence is playing, so gameplay can hide its HUD and hold its input during a cinematic.
        FUNCTION()
        bool IsAnySequencePlaying();

        // A fade toward Color and black bars at LetterboxAspect over whichever camera is live, zero clearing each.
        FUNCTION()
        void SetCinematicOverlay(float Fade, FVector3 Color, float LetterboxAspect);
    };
}
