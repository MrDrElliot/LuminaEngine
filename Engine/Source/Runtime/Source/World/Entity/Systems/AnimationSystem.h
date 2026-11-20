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

    // What the drawn followers ask of one leader, the latest time one was drawn and the finest detail any needs.
    struct FAnimLeaderDemand
    {
        double FollowerRenderedTime = 0.0;
        float  DistanceOverRadius   = 0.0f;
        float  BoneLODTolerance     = 0.0f;
    };

    REFLECT()
    class CAnimationSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SSimpleAnimationComponent>, ECS::Write<SSkeletalMeshComponent>, ECS::Without<SDisabledTag>, ECS::Without<SFollowerPoseComponent>> SimpleAnimators;
        ECS::TQuery<ECS::Write<SAnimationGraphComponent>, ECS::Write<SSkeletalMeshComponent>, ECS::Without<SDisabledTag>, ECS::Without<SFollowerPoseComponent>> GraphAnimators;
        ECS::TQuery<ECS::Write<SSkeletalMeshComponent>, ECS::Without<SDisabledTag>> Meshes;
        ECS::TQuery<ECS::Write<SFollowerPoseComponent>, ECS::Write<SSkeletalMeshComponent>, ECS::Without<SDisabledTag>> Followers;
        ECS::TQuery<ECS::Read<SFollowerPoseComponent>> PoseLinks;
        ECS::TQuery<ECS::Write<SSkeletalMeshComponent>> LeaderMeshes;
        TRes<FKinematicsState> KinematicsState;
        TRes<SWorldSettingsState> WorldSettings;
        ECS::TEventWriter<SAnimMontageStartedEvent> MontageStarted;
        ECS::TEventWriter<SAnimMontageBlendingOutEvent> MontageBlendingOut;
        ECS::TEventWriter<SAnimMontageEndedEvent> MontageEnded;
        ECS::TEventWriter<SAnimNotifyFiredEvent> NotifyFired;
        THashSet<ECS::FEntity> TakenFromFollowers;

        // Gathered per worker, so the pass over every follower runs in parallel.
        TVector<THashMap<ECS::FEntity, FAnimLeaderDemand>> LeaderDemandBySlot;

        // Notify handlers never run inside this system, so its declared access only has to cover the passes.
        FAnimNotifyQueue NotifyQueue;
    };
}
