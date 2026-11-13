#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "HierarchicalTags/HierarchicalTagComponent.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/PathFollowComponent.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/StateTreeComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "StateTreeSystem.generated.h"

namespace Lumina
{
    // Starts and ticks every entity's state tree in parallel chunks; a sleeping instance costs one wake time check.
    REFLECT()
    class RUNTIME_API CStateTreeSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;
        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SStateTreeComponent>> Trees;

        // A lookup only, declaring what the library tasks touch on their own entity and read on the entities they target.
        ECS::TQuery<ECS::Write<SPathFollowComponent>, ECS::Write<SAnimationGraphComponent>, ECS::Read<STransformComponent>,
            ECS::Read<SPerceptionComponent>, ECS::Read<SHierarchicalTagComponent>> NodeAccess;
    };
}
