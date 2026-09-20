#pragma once

#include "World/ECS/Registry.h"

#include "AI/Avoidance/CrowdSolver.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "EntitySystem.h"
#include "RVOAvoidanceSystem.generated.h"

namespace Lumina
{
    // Per-world avoidance state published into the registry context.
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
    class RUNTIME_API SRVOAvoidanceSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnStartup() override;
        void OnUpdate() override;
    };

    namespace Avoidance
    {
        RUNTIME_API const FAvoidanceState* GetState(const FSystemContext& Context);
    }
}
