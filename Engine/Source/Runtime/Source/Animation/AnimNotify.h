#pragma once

#include "World/ECS/Registry.h"

#include "Core/Object/ObjectMacros.h"
#include "AnimNotify.generated.h"

namespace Lumina
{
    // One instance is shared by every entity playing the clip, so keep per-entity state on the entity.
    REFLECT()
    struct RUNTIME_API SAnimNotify
    {
        GENERATED_BODY()

        virtual ~SAnimNotify() = default;

        // Runs on the main thread after the animation batch, so it may add, remove or write any component.
        virtual void Notify(ECS::FRegistry& Registry, ECS::FEntity Entity) const {}
    };

    // Ranged notify, same sharing rule as SAnimNotify.
    REFLECT()
    struct RUNTIME_API SAnimNotifyState
    {
        GENERATED_BODY()

        virtual ~SAnimNotifyState() = default;

        // Every callback runs on the main thread after the animation batch, as SAnimNotify::Notify does.
        virtual void NotifyBegin(ECS::FRegistry& Registry, ECS::FEntity Entity) const {}

        // Alpha is 0..1 across the window.
        virtual void NotifyTick(ECS::FRegistry& Registry, ECS::FEntity Entity, float Alpha) const {}

        virtual void NotifyEnd(ECS::FRegistry& Registry, ECS::FEntity Entity) const {}
    };
}
