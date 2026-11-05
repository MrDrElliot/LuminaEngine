#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/InterpolatingMovementComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "MovementComponentSystem.generated.h"

namespace Lumina
{
    // Drives SInterpolatingMovementComponent and SSineWaveMovementComponent in game worlds.
    REFLECT()
    class RUNTIME_API SMovementComponentSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SInterpolatingMovementComponent>, ECS::Write<STransformComponent>, ECS::Without<SDisabledTag>> Interpolators;
        ECS::TQuery<ECS::Write<SSineWaveMovementComponent>, ECS::Write<STransformComponent>, ECS::Without<SDisabledTag>> Oscillators;
    };
}
