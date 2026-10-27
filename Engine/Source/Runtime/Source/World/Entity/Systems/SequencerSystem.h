#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "SequencerSystem.generated.h"

namespace Lumina
{
    // Advances every SSequencePlayerComponent and writes its tracks into the world, ahead of the animation and scripts that read those poses.
    REFLECT()
    class SSequencerSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

    public:

        void OnUpdate(const SSystemContext& Context) override;
        bool HasWork(const SSystemContext& Context) override;
    };
}
