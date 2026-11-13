#pragma once
#include "EntitySystem.h"
#include "Animation/AnimNotifyQueue.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/FollowerPoseComponent.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/KinematicsSystem.h"
#include "World/Subsystems/WorldSettings.h"
#include "AnimationSystem.generated.h"

namespace Lumina
{

    REFLECT()
    class CAnimationSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SSimpleAnimationComponent>, ECS::Write<SSkeletalMeshComponent>, ECS::Without<SDisabledTag>, ECS::Without<SFollowerPoseComponent>> SimpleAnimators;
        ECS::TQuery<ECS::Write<SAnimationGraphComponent>, ECS::Write<SSkeletalMeshComponent>, ECS::Optional<ECS::Read<STransformComponent>>, ECS::Without<SDisabledTag>, ECS::Without<SFollowerPoseComponent>> GraphAnimators;
        ECS::TQuery<ECS::Write<SSkeletalMeshComponent>, ECS::Without<SDisabledTag>> Meshes;
        ECS::TQuery<ECS::Write<SFollowerPoseComponent>, ECS::Write<SSkeletalMeshComponent>, ECS::Without<SDisabledTag>> Followers;
        ECS::TQuery<ECS::Read<SFollowerPoseComponent>> PoseLinks;
        ECS::TQuery<ECS::Write<SSkeletalMeshComponent>> LeaderMeshes;
        TRes<FKinematicsState> KinematicsState;
        TRes<FWorldSettingsState> WorldSettings;
        ECS::TEventWriter<SAnimMontageStartedEvent> MontageStarted;
        ECS::TEventWriter<SAnimMontageBlendingOutEvent> MontageBlendingOut;
        ECS::TEventWriter<SAnimMontageEndedEvent> MontageEnded;
        THashSet<ECS::FEntity> TakenFromFollowers;

        // Notify handlers never run inside this system, so its declared access only has to cover the passes.
        FAnimNotifyQueue NotifyQueue;
    };
}
