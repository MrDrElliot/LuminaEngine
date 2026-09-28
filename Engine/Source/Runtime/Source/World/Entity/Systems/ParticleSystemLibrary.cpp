#include "RuntimePCH.h"

#include "ParticleSystemLibrary.h"

#include "Animation/SkeletalMeshLibrary.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/World.h"

namespace Lumina
{
    static float ResolveLifetime(const CParticleSystem* ParticleSystem, float Lifetime)
    {
        return Lifetime < 0.0f ? ParticleSystem->GetOneShotLength() : Lifetime;
    }

    ECS::FEntity CParticleSystemLibrary::SpawnParticleSystem(CWorld* World, CParticleSystem* ParticleSystem,
        const FTransform& SpawnTransform, float Lifetime)
    {
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

    ECS::FEntity CParticleSystemLibrary::SpawnParticleSystemAttached(CWorld* World,
        CParticleSystem* ParticleSystem, ECS::FEntity Parent, const FName& Socket, FVector3 Offset, float Lifetime)
    {
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
        if (Socket.IsNone() || FStringView(Socket.c_str()).empty())
        {
            World->SetParent(Spawned, Parent);
        }
        else
        {
            CSkeletalMeshLibrary::AttachEntityToSocket(World, Spawned, Parent, Socket);
        }

        World->SetEntityLifetime(Spawned, ResolveLifetime(ParticleSystem, Lifetime));
        return Spawned;
    }

    void CParticleSystemLibrary::GetCollisions(CWorld* World, ECS::FEntity Entity, int32 EmitterIndex, TVector<FParticleCollision>& OutCollisions)
    {
        if (World == nullptr)
        {
            return;
        }

        const SParticleSystemComponent* Effect = World->TryGetComponent<SParticleSystemComponent>(Entity);
        if (Effect == nullptr)
        {
            return;
        }

        for (const FParticleCollision& Hit : Effect->Collisions)
        {
            if (EmitterIndex < 0 || Hit.EmitterIndex == EmitterIndex)
            {
                OutCollisions.push_back(Hit);
            }
        }
    }
}
