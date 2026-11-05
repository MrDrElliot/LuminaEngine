#pragma once
#include "ExclusiveSystem.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/SequencePlayerComponent.h"
#include "SequencerSystem.generated.h"

namespace Lumina
{

    // Advances every SSequencePlayerComponent and writes its tracks into the world, ahead of the animation and scripts that read those poses.
    REFLECT()
    class SSequencerSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnExclusiveUpdate(const SExclusiveContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SSequencePlayerComponent>> Players;
    };
}
