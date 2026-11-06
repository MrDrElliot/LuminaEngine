#pragma once

#include "World/Entity/Systems/EntitySystem.h"
#include "Components/HealthComponent.h"
#include "HealthSystem.generated.h"

namespace Lumina
{

    // Drives SHealthComponent::RegenPerSecond and the post-damage cooldown that gates it.
    REFLECT()
    class CHealthSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SHealthComponent>> Regenerators;
    };
}
