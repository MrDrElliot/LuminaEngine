#include "RuntimePCH.h"

#include "World/Subsystems/SpawnSubsystem.h"

#include "World/ECS/Registry.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/World.h"

namespace Lumina
{
    static float ResolveLifetime(const CParticleSystem* ParticleSystem, float Lifetime)
    {
        return Lifetime < 0.0f ? ParticleSystem->GetOneShotLength() : Lifetime;
    }

    ECS::FEntity CSpawnSubsystem::SpawnParticleSystem(CParticleSystem* ParticleSystem,
        const FTransform& SpawnTransform, float Lifetime)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || ParticleSystem == nullptr)
        {
            return ECS::NullEntity;
        }

        const ECS::FEntity Spawned = World->ConstructEntity("ParticleEffect", SpawnTransform);
        SParticleSystemComponent& Effect = World->EmplaceComponent<SParticleSystemComponent>(Spawned);
        Effect.ParticleSystem = ParticleSystem;
        Effect.bBurstOnSpawn = true;
        Effect.Activate(true);

        World->SetEntityLifetime(Spawned, ResolveLifetime(ParticleSystem, Lifetime));
        return Spawned;
    }

    ECS::FEntity CSpawnSubsystem::SpawnParticleSystemAttached(CParticleSystem* ParticleSystem, ECS::FEntity Parent, const FName& Socket, FVector3 Offset, float Lifetime)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || ParticleSystem == nullptr || !World->IsValidEntity(Parent))
        {
            return ECS::NullEntity;
        }

        // Attaching snaps the child onto the socket, so spawning at a world point first would be undone.
        const ECS::FEntity Spawned = World->ConstructEntity("ParticleEffect", FTransform());
        SParticleSystemComponent& Effect = World->EmplaceComponent<SParticleSystemComponent>(Spawned);
        Effect.ParticleSystem = ParticleSystem;
        Effect.EmitterOffset = Offset;
        Effect.bBurstOnSpawn = true;
        Effect.Activate(true);

        // A managed caller spells "no socket" as an empty string, which does not intern to NAME_None.
        if (Socket.IsNone() || FStringView(*Socket).IsEmpty())
        {
            World->SetParent(Spawned, Parent);
        }
        else
        {
            World->GetAnimation()->AttachEntityToSocket(Spawned, Parent, Socket);
        }

        World->SetEntityLifetime(Spawned, ResolveLifetime(ParticleSystem, Lifetime));
        return Spawned;
    }

    void CSpawnSubsystem::GetParticleCollisions(ECS::FEntity Entity, int32 EmitterIndex, TVector<SParticleCollision>& OutCollisions)
    {
        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            return;
        }

        const SParticleSystemComponent* Effect = World->TryGetComponent<ECS::Read<SParticleSystemComponent>>(Entity);
        if (Effect == nullptr)
        {
            return;
        }

        for (const SParticleCollision& Hit : Effect->Collisions)
        {
            if (EmitterIndex < 0 || Hit.EmitterIndex == EmitterIndex)
            {
                OutCollisions.Add(Hit);
            }
        }
    }
}
