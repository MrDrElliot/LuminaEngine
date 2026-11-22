#pragma once

#include "World/Entity/Systems/EntitySystem.h"
#include "Components/HealthComponent.h"
#include "HealthSystem.generated.h"

namespace Lumina
{

    // Drives SHealthComponent::RegenPerSecond on the host, and on a client raises its events when replicated health arrives.
    REFLECT()
    class CHealthSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        void Regenerate(const SSystemContext& Context);

        ECS::TQuery<ECS::Write<SHealthComponent>> Regenerators;
        ECS::TQuery<ECS::Read<SHealthComponent>, ECS::Changed<SHealthComponent>> ChangedHealth;
    };
}
