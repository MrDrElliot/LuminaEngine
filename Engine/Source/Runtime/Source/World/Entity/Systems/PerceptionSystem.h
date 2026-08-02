#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Delegates/Delegate.h"
#include "AI/Perception/PerceptionTypes.h"
#include "PerceptionSystem.generated.h"

namespace Lumina
{
    // Broadcast when a perceiver first becomes aware of a target, or loses it. Carries the perceiver and the
    // perceived-target record. C++ listeners bind via AddStatic/AddMember; C# overrides EntityScript
    // OnTargetPerceived/OnTargetLost; in-ECS systems subscribe to FPerceptionUpdatedEvent on the dispatcher.
    DECLARE_MULTICAST_DELEGATE(FOnPerceptionUpdated, entt::entity, const FPerceivedTarget&);

    // Runs every perceiver's senses each tick: a spatial-grid sight scan with FOV + line-of-sight (parallel),
    // event-driven hearing/damage, hysteresis + forgetting, and the perceived/lost events. PrePhysics, ahead
    // of PathFollow, so an AI can react and set a move target the same frame; bodies are stable for LoS rays.
    REFLECT(System)
    struct RUNTIME_API SPerceptionSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics, EUpdatePriority::High))

        static FSystemAccess Access;

        // Creates the per-world perception singleton once, serially at world init -- Update runs concurrently
        // with sibling systems, so it must never structurally mutate the shared registry context.
        static void Startup(const FSystemContext& Context) noexcept;

        static void Update(const FSystemContext& Context) noexcept;

        static FOnPerceptionUpdated OnTargetPerceived;
        static FOnPerceptionUpdated OnTargetLost;
    };
}
