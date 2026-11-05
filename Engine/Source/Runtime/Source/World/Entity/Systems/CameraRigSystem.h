#pragma once
#include "EntitySystem.h"
#include "World/Entity/Components/CameraFollowComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/SpringArmComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "CameraRigSystem.generated.h"

namespace Lumina
{

    // Places rig cameras (follow, spring arm) on settled transforms before SCameraSystem; with both, the spring arm wins.
    REFLECT()
    class SCameraRigSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        // The rigs decide, since the target lookup matches nearly every entity.
        bool HasWork(const SSystemContext& Context) override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SCameraFollowComponent>, ECS::Write<STransformComponent>, ECS::Without<SDisabledTag>, ECS::Without<SSpringArmComponent>> Followers;
        ECS::TQuery<ECS::Write<SSpringArmComponent>, ECS::Write<STransformComponent>, ECS::Without<SDisabledTag>> SpringArms;
        ECS::TQuery<ECS::Read<STransformComponent>> Targets;
    };
}
