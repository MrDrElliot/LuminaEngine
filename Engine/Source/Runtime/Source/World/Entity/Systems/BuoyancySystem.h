#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/BuoyancyComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "BuoyancySystem.generated.h"

namespace Lumina
{

    REFLECT()
    class RUNTIME_API SBuoyancySystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;
        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Read<SWaterComponent>, ECS::Read<STransformComponent>> Waters;
        ECS::TQuery<ECS::Read<SBuoyancyComponent>, ECS::Read<SRigidBodyComponent>> Floaters;
    };
}
