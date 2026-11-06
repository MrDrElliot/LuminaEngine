#pragma once

#include "EntitySystem.h"
#include "SignificanceSystem.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Threading/Thread.h"
#include "World/ECS/Entity.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/Entity/Components/PathFollowComponent.h"
#include "World/Entity/Components/RVOAgentComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/VehicleComponent.h"
#include "PathFollowSystem.generated.h"

namespace Lumina
{
    class FNavMesh;

    // Follows each agent's cached path and writes a move direction into its controller, in PrePhysics so input lands the same step.
    REFLECT()
    class RUNTIME_API SPathFollowSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        bool HasWork(const SSystemContext& Context) override;
        void OnUpdate(const SSystemContext& Context) override;

    private:

        struct FAgentMesh
        {
            FName       Agent;
            FNavMesh*   Mesh = nullptr;
        };

        static FNavMesh* MeshForAgent(const TVector<FAgentMesh>& Meshes, FName Agent);

        struct FRepathRequest
        {
            ECS::FEntity    Entity;
            FVector3        Goal;
            FVector3        AgentPos;
        };

        // One per worker, so the first pass can set aside the followers that owe a path query without contention.
        struct CACHE_ALIGN FRepathBucket
        {
            TVector<FRepathRequest> Requests;
        };

        ECS::TQuery<ECS::Write<SPathFollowComponent>,
            ECS::Read<STransformComponent>,
            ECS::Optional<ECS::Write<SVehicleComponent>>,
            ECS::Optional<ECS::Write<SRVOAgentComponent>>,
            ECS::Optional<ECS::Write<SCharacterControllerComponent>>,
            ECS::Optional<ECS::Read<SCharacterMovementComponent>>> Followers;

        ECS::TQuery<ECS::Read<SNavMeshComponent>> NavMeshes;

        // A lookup only, for the transform of a follower's target entity.
        ECS::TQuery<ECS::Read<STransformComponent>> Transforms;

        TRes<FSignificanceState> SignificanceState;

        TVector<FAgentMesh>     AgentMeshes;
        TVector<FRepathBucket>  RepathBuckets;
        TVector<FRepathRequest> RepathQueue;

        // Index of the last follower served when the repath budget ran out, so the next tick starts after it.
        uint32                  RepathCursor = 0;
    };
}
