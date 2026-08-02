#pragma once
#include "EntitySystem.h"
#include "InputSystem.generated.h"

namespace Lumina
{
    // Samples each world's OWN input context once per frame and writes a per-entity snapshot onto every
    // SInputComponent. Runs at Highest priority (before scripts/gameplay) so queries read stable data, not
    // live globals -- killing the active-vs-per-world routing inconsistency and the cross-world mouse leak.
    // Because it resolves the world's own viewport, it behaves identically in editor and shipping, and the
    // snapshot is the per-entity input command we can later serialize for networked play.
    REFLECT(System)
    struct SInputSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(  RequiresUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest),
                        RequiresUpdate(EUpdateStage::PrePhysics, EUpdatePriority::Highest))

        // Writes only the per-entity input snapshot; the viewport registry it reads is a process global
        // accessed read-only. Disjoint from gameplay/physics → overlaps them. Defined in the .cpp.
        static FSystemAccess Access;

        static void Update(const FSystemContext& Context) noexcept;
    };
}
