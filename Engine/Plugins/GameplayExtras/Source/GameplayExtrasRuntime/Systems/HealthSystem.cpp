#include "HealthSystem.h"
#include "World/ECS/Registry.h"

#include "Components/HealthComponent.h"
#include "Core/Math/Math.h"
#include "World/WorldTypes.h"

namespace Lumina
{
    // Heal broadcasts into script, so it runs deferred to the main thread and the countdown is all this system touches.
    void CHealthSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
    }

    void CHealthSystem::OnUpdate(const SSystemContext& Context)
    {
        if (Context.GetWorldType() == EWorldType::Editor)
        {
            return;
        }

        // A client only hears health from the host, so it regenerates nothing and turns each arrival into the events a mutator would raise.
        if (!Context.IsNetClient())
        {
            Regenerate(Context);
            return;
        }

        TVector<ECS::FEntity> Arrived;
        ChangedHealth.ForEach([&](ECS::FEntity Entity, const SHealthComponent& Health)
        {
            if (Health.HasUnobservedChange())
            {
                Arrived.Add(Entity);
            }
        });
        if (Arrived.IsEmpty())
        {
            return;
        }

        Context.Defer([Arrived = Move(Arrived)](const SExclusiveContext& X)
        {
            ECS::FRegistry& Registry = X.GetRegistry();
            for (ECS::FEntity Entity : Arrived)
            {
                SHealthComponent* Live = Registry.IsValid(Entity) ? Registry.TryGet<ECS::Write<SHealthComponent>>(Entity) : nullptr;
                if (Live != nullptr)
                {
                    Live->BroadcastArrival();
                }
            }
        });
    }

    void CHealthSystem::Regenerate(const SSystemContext& Context)
    {
        const float DeltaTime = Context.GetDeltaTime();
        if (DeltaTime <= 0.0f)
        {
            return;
        }

        Regenerators.ForEach([&](ECS::FEntity Entity, ECS::TMut<SHealthComponent> HealthMut)
        {
            const SHealthComponent& Current = HealthMut.Get();
            if (Current.bDead || !Current.RegenPerSecond.IsSet() || Current.Health >= Current.MaxHealth)
            {
                return;
            }
            SHealthComponent& Health = HealthMut.GetMut();

            if (Health.RegenCooldown > 0.0f)
            {
                Health.RegenCooldown -= DeltaTime;
                return;
            }

            const float Amount = Health.RegenPerSecond.GetValue() * DeltaTime;
            Context.Defer([Entity, Amount](const SExclusiveContext& X)
            {
                ECS::FRegistry& Registry = X.GetRegistry();
                SHealthComponent* Live = Registry.IsValid(Entity) ? Registry.TryGet<ECS::Write<SHealthComponent>>(Entity) : nullptr;
                if (Live != nullptr && !Live->bDead)
                {
                    Live->Heal(Amount, ECS::NullEntity);
                }
            });
        });
    }
}
