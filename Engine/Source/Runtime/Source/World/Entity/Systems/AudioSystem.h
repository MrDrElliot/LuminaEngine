#pragma once

#include "EntitySystem.h"
#include "AudioSystem.generated.h"

namespace Lumina
{
    REFLECT(System)
    struct SAudioSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PostPhysics))

        // Writes the audio source/procedural components; reads transforms + listeners. The audio device
        // (GAudioContext) is a process global, but this is the only system that touches it within a world,
        // so within-world batching is safe. Defined in the .cpp.
        static FSystemAccess Access;

        static void Startup(const FSystemContext& Context) noexcept;
        static void Update(const FSystemContext& Context) noexcept;
        static void Teardown(const FSystemContext& Context) noexcept;
    };
}
