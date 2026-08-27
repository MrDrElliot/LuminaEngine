#pragma once

#include "World/ECS/Registry.h"

#include "Containers/Vector.h"
#include "EntitySystem.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "SignificanceSystem.generated.h"

namespace Lumina
{
    class CWorld;

    struct FEntitySignificance
    {
        // Stamped so a recycled entity slot never hands back the previous occupant's score.
        ECS::FEntity    Owner = ECS::NullEntity;

        // Frame this was written, so an entity that stops being scored stops reading as scored.
        uint32          Stamp = 0;

        float           DistanceSq = 0.0f;

        // Distance over world bounding radius, so a large object stays significant further out.
        float           DistanceOverRadius = 0.0f;

        // Frames a consumer should aim to leave between updates, 1 meaning every frame.
        uint8           TickInterval = 1;

        // Deliberately not folded into TickInterval, because an AI behind the camera still has to think.
        bool            bInView = true;
    };

    // Per-world scores published into the registry context ahead of every consumer.
    struct FSignificanceState
    {
        // Indexed by ECS::FEntity::GetIndex(), sparse, an entry whose Owner mismatches is unscored.
        TVector<FEntitySignificance> ByEntityIndex;

        FVector3    ViewOrigin = FVector3(0.0f);

        // False when no camera resolved, which reports full significance rather than throttling the world.
        bool        bHasView = false;

        bool        bEnabled = true;

        // Bumped once per scoring pass; an entry from an older pass reads as unscored.
        uint32      Stamp = 0;

        // Full significance for anything unscored, so a consumer that finds nothing keeps ticking.
        NODISCARD FEntitySignificance Get(ECS::FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            if (!bEnabled || !bHasView || Index >= (uint32)ByEntityIndex.size())
            {
                return FEntitySignificance{};
            }

            const FEntitySignificance& Entry = ByEntityIndex[Index];
            const bool bFresh = Entry.Owner == Entity && Entry.Stamp == Stamp;
            return bFresh ? Entry : FEntitySignificance{};
        }
    };

    // Scores every entity carrying a transform once per frame; the scores live in the registry context.
    REFLECT(System)
    struct RUNTIME_API SSignificanceSystem
    {
        GENERATED_BODY()
        // Paused as well as FrameStart, since editor worlds tick only Paused. Highest so it lands first.
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest),
                      RequiresUpdate(EUpdateStage::Paused,     EUpdatePriority::Highest))

    public:

        static FSystemAccess Access;

        static void Startup(const FSystemContext& Context) noexcept;
        static void Update (const FSystemContext& Context) noexcept;
    };

    namespace Significance
    {
        // Reference radius for a meshless entity, which puts the bands at 30, 60 and 120 meters.
        inline constexpr float kDefaultRadius = 1.0f;

        RUNTIME_API const FSignificanceState* GetState(const FSystemContext& Context);
        RUNTIME_API const FSignificanceState* GetState(CWorld* World);

        // The one definition of the tick bands, shared with the metric SAnimationSystem gets from the gather.
        NODISCARD FORCEINLINE uint8 IntervalForDistanceOverRadius(float DistanceOverRadius)
        {
            if (DistanceOverRadius > 120.0f)
            {
                return 4;
            }
            if (DistanceOverRadius > 60.0f)
            {
                return 3;
            }
            if (DistanceOverRadius > 30.0f)
            {
                return 2;
            }
            return 1;
        }

        // Returns BaseInterval unchanged with no state, no view or a nearby entity, so callers need no guard.
        NODISCARD FORCEINLINE float ScaleInterval(const FSignificanceState* State, ECS::FEntity Entity, float BaseInterval)
        {
            if (State == nullptr)
            {
                return BaseInterval;
            }
            return BaseInterval * (float)State->Get(Entity).TickInterval;
        }
    }
}
