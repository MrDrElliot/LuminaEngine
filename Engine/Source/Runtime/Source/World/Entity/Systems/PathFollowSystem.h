#pragma once

#include "EntitySystem.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Threading/Thread.h"
#include "World/ECS/Entity.h"
#include "PathFollowSystem.generated.h"

namespace Lumina
{
    // Ticks every SPathFollowComponent: refreshes the cached path (Nav::FindPath), advances the corner cursor,
    // and writes a move direction into the paired controller. In PrePhysics so input lands the same step.
    REFLECT()
    class RUNTIME_API SPathFollowSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate() override;

    private:

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

        TVector<FRepathBucket>  RepathBuckets;
        TVector<FRepathRequest> RepathQueue;
    };
}
