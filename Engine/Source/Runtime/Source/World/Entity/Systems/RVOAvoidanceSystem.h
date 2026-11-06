#pragma once

#include "World/ECS/Registry.h"

#include "AI/Avoidance/CrowdSolver.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "EntitySystem.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/Entity/Components/RVOAgentComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/SignificanceSystem.h"
#include "World/Entity/Systems/SystemResources.h"
#include "RVOAvoidanceSystem.generated.h"

namespace Lumina
{
    // Per-world avoidance scratch and the last solve's counts.
    struct FAvoidanceState
    {
        Avoidance::FCrowdAgents Agents;

        // Parallel to the solver's arrays, and the only part of the scratch that knows about the ECS.
        TVector<ECS::FEntity>   Entities;

        // Agents that asked for the navmesh clamp, as indices into the solver's arrays.
        TVector<int32>          NavClampList;

        uint32 FrameCounter = 0;
        bool   bEnabled     = true;

        int32  LastAgentCount  = 0;
        int32  LastSolvedCount = 0;
        int32  LastNavClamped  = 0;
    };

    // Reciprocal velocity obstacle avoidance over every entity carrying SRVOAgentComponent.
    REFLECT()
    class RUNTIME_API CRVOAvoidanceSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        // Only the avoiders decide, since the other queries are lookups.
        bool HasWork(const SSystemContext& Context) override;
        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SRVOAgentComponent>, ECS::Read<STransformComponent>, ECS::Optional<ECS::Read<SCharacterPhysicsComponent>>, ECS::Optional<ECS::Read<SCharacterMovementComponent>>, ECS::Optional<ECS::Write<SCharacterControllerComponent>>> Avoiders;
        ECS::TQuery<ECS::Read<SNavMeshComponent>> NavMeshes;
        TRes<FSignificanceState> SignificanceState;

        FAvoidanceState State;
    };
}
