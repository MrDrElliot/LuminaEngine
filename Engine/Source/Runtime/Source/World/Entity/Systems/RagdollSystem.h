#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "RagdollSystem.generated.h"

namespace Lumina
{

    // Creates ragdolls from the animated pose in PrePhysics and reads the simulated bodies back into the bones in PostPhysics.
    REFLECT()
    class SRagdollSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

    public:

        // Only the ragdolls, since the pose and transform writers match almost every mesh.
        bool HasWork(const SSystemContext& Context) override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SRagdollComponent>, ECS::Read<SSkeletalMeshComponent>, ECS::Read<STransformComponent>> Ragdolls;
        ECS::TQuery<ECS::Write<SSkeletalMeshComponent>> PoseTargets;
        ECS::TQuery<ECS::Write<STransformComponent>> DrivenTransforms;
    };
}
