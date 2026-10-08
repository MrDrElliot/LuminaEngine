#pragma once
#include "EntitySystem.h"
#include "Animation/AnimNotifyQueue.h"
#include "Core/Object/ObjectMacros.h"
#include "AnimationSystem.generated.h"

namespace Lumina
{
    REFLECT()
    class SAnimationSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;
        void OnUpdate() override;

    private:

        // Notify handlers never run inside this system, so its declared access only has to cover the passes.
        FAnimNotifyQueue NotifyQueue;
    };
}
