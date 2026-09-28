#pragma once

#include "Assets/AssetTypes/Sequence/Sequence.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "SequenceLibrary.generated.h"

namespace Lumina
{
    class CSequence;
    class CWorld;

    // Playing cinematics from script, and hearing the events they fire.
    REFLECT()
    class RUNTIME_API CSequenceLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // Spawns a player entity for Sequence and starts it, returning the player so it can be paused, sought or stopped.
        FUNCTION()
        static ECS::FEntity PlaySequence(CWorld* World, CSequence* Sequence, bool bLoop = false, float PlayRate = 1.0f);

        // Stops the player and destroys its entity, putting back whatever the sequence displaced.
        FUNCTION()
        static void StopSequence(CWorld* World, ECS::FEntity Player);

        FUNCTION()
        static bool IsSequencePlaying(CWorld* World, ECS::FEntity Player);

        // Every event any sequence in the world fired since the last call, oldest first.
        static void ConsumeEvents(CWorld* World, TVector<SSequenceFiredEvent>& OutEvents);

        // Steps to the next event any sequence fired, false once there are none, then the getters below describe it.
        FUNCTION()
        static bool NextEvent(CWorld* World);

        FUNCTION()
        static FString GetEventName(CWorld* World);

        FUNCTION()
        static FString GetEventPayload(CWorld* World);

        FUNCTION()
        static ECS::FEntity GetEventTarget(CWorld* World);

        FUNCTION()
        static float GetEventTime(CWorld* World);

        // Whether any sequence is playing, so gameplay can hide its HUD and hold its input during a cinematic.
        FUNCTION()
        static bool IsAnySequencePlaying(CWorld* World);

        // A fade toward Color and black bars at LetterboxAspect over whichever camera is live, zero clearing each.
        FUNCTION()
        static void SetCinematicOverlay(CWorld* World, float Fade, FVector3 Color, float LetterboxAspect);
    };
}
