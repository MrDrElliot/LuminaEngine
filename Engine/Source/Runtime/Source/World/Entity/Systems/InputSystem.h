#pragma once
#include "EntitySystem.h"
#include "InputSystem.generated.h"

namespace Lumina
{
    // Delivers input to the scripts on every entity tagged with an SInputComponent: discrete events through
    // OnInput, and declarative bindings through the per-frame action states. Runs at Highest priority so it
    // is ahead of gameplay. It stores nothing: the state lives once per viewport in FInputContext and is read
    // through Input:: (Input/InputQuery.h).
    REFLECT(System)
    struct SInputSystem
    {
        GENERATED_BODY()
        // FrameStart only. FInputContext::FrameEvents is not cleared until EndFrame, so a second stage
        // re-delivered every event to every script and rebuilt the snapshot a second time.
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest))

        // Reads the routing tag only; the viewport registry it consults is a process global accessed
        // read-only. Disjoint from gameplay and physics, so it overlaps them. Defined in the .cpp.
        static FSystemAccess Access;

        static void Update(const FSystemContext& Context) noexcept;
    };
}
