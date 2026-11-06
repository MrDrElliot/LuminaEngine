#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/SequencePlayerComponent.h"
#include "SequencerSystem.generated.h"

namespace Lumina
{

    // Advances every SSequencePlayerComponent and writes its tracks into the world, ahead of the animation and scripts that read those poses.
    REFLECT()
    class CSequencerSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        void Advance(const SExclusiveContext& Context);

        ECS::TQuery<ECS::Write<SSequencePlayerComponent>> Players;
    };
}
