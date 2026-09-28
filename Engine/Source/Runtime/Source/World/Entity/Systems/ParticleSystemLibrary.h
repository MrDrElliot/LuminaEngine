#pragma once

#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Transform.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "World/ECS/Entity.h"
#include "ParticleSystemLibrary.generated.h"

namespace Lumina
{
    class CParticleSystem;
    class CWorld;

    /** One-shot effect spawning, which is the particle system's business rather than the world's. */
    REFLECT()
    class RUNTIME_API CParticleSystemLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // Bursts at SpawnTransform and despawns after Lifetime seconds, where zero leaves it to the caller and negative means the one-shot length.
        FUNCTION()
        static ECS::FEntity SpawnParticleSystem(CWorld* World, CParticleSystem* ParticleSystem,
            const FTransform& SpawnTransform, float Lifetime);

        /** The same, parented to Parent on a named socket or bone, where none means the entity origin. */
        FUNCTION()
        static ECS::FEntity SpawnParticleSystemAttached(CWorld* World, CParticleSystem* ParticleSystem,
            ECS::FEntity Parent, const FName& Socket, FVector3 Offset, float Lifetime);
    };
}
