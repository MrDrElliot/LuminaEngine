#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "SequencerSystem.generated.h"

namespace Lumina
{
    // Advances every SSequencePlayerComponent and writes its tracks into the world. Runs on PrePhysics at
    // high priority: a sequence poses entities, and animation, physics and the camera all read those poses
    // later in the same frame.
    REFLECT()
    class SSequencerSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

    public:

        void OnUpdate() override;
        bool HasWork(EUpdateStage Stage) override;
    };
}
