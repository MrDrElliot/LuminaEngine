#pragma once

#include "Core/Object/ObjectMacros.h"
#include "World/Entity/EntityHandle.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "AnimNotify.generated.h"

namespace Lumina
{
    // One instance is shared by every entity playing the clip, so keep per-entity state on the entity.
    REFLECT()
    struct RUNTIME_API SAnimNotify
    {
        GENERATED_BODY()

        virtual ~SAnimNotify() = default;

        virtual void Notify(FEntityRegistry& Registry, FEntity Entity) const {}
    };

    // Ranged notify, same sharing rule as SAnimNotify.
    REFLECT()
    struct RUNTIME_API SAnimNotifyState
    {
        GENERATED_BODY()

        virtual ~SAnimNotifyState() = default;

        virtual void NotifyBegin(FEntityRegistry& Registry, FEntity Entity) const {}

        // Alpha is 0..1 across the window.
        virtual void NotifyTick(FEntityRegistry& Registry, FEntity Entity, float Alpha) const {}

        virtual void NotifyEnd(FEntityRegistry& Registry, FEntity Entity) const {}
    };
}
