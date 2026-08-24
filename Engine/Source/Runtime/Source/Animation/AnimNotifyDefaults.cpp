#include "RuntimePCH.h"
#include "AnimNotifyDefaults.h"

#include "Animation/SkeletalMeshUtils.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Audio/AudioGlobals.h"
#include "Log/Log.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/ParticleSystemComponent.h"

namespace Lumina
{
    namespace
    {
        // Socket space when one resolves, else the entity's world transform, else nothing to place it on.
        bool ResolveNotifyWorldTransform(FEntityRegistry& Registry, FEntity Entity, const FString& Socket, FTransform& OutTransform)
        {
            FMatrix4 SocketTransform;
            if (!Socket.empty() && SkeletalUtils::GetSocketWorldTransform(Registry, Entity, FName(Socket.c_str()), SocketTransform))
            {
                OutTransform = FTransform(SocketTransform);
                return true;
            }

            STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity);
            if (Transform == nullptr)
            {
                return false;
            }

            OutTransform = Transform->GetWorldTransform();
            return true;
        }

        // The offset rides in the socket's space, so it follows the bone's orientation.
        bool ResolveNotifyWorldPoint(FEntityRegistry& Registry, FEntity Entity, const FString& Socket,
                                     const FVector3& Offset, FVector3& OutPoint)
        {
            FTransform Placement;
            if (!ResolveNotifyWorldTransform(Registry, Entity, Socket, Placement))
            {
                return false;
            }

            OutPoint = Placement.GetLocation() + Placement.GetRotation() * Offset;
            return true;
        }
    }

    void SAnimNotify_PlaySound::Notify(FEntityRegistry& Registry, FEntity Entity) const
    {
        if (!Audio::HasDevice() || Sound == nullptr || !Sound->IsValid())
        {
            return;
        }

        FAudioPlayParams Params{};
        Params.Volume      = Volume;
        Params.Pitch       = Pitch;
        Params.Bus         = Bus;
        Params.Attenuation = Attenuation.Resolve();
        Params.bSpatialized = bSpatialized;

        if (bSpatialized && !ResolveNotifyWorldPoint(Registry, Entity, Socket, Offset, Params.Position))
        {
            // No transform to place it at, so a spatialized voice would sit at the origin.
            Params.bSpatialized = false;
        }

        (void)Audio::Context().PlayAudio(Sound->GetAudioData(), Params);
    }

    void SAnimNotify_PlayParticleSystem::Notify(FEntityRegistry& Registry, FEntity Entity) const
    {
        if (ParticleSystem == nullptr)
        {
            return;
        }

        // The world lives in the registry's context singleton, which is how the script driver reaches it too.
        CWorld** WorldPtr = Registry.ctx().find<CWorld*>();
        CWorld* World = WorldPtr != nullptr ? *WorldPtr : nullptr;
        if (World == nullptr)
        {
            return;
        }

        if (bAttachToSocket && !Socket.empty())
        {
            World->SpawnParticleSystemAttached(ParticleSystem, Entity, FName(Socket.c_str()), Offset, Lifetime);
            return;
        }

        FTransform SpawnTransform;
        if (!ResolveNotifyWorldTransform(Registry, Entity, Socket, SpawnTransform))
        {
            return;
        }

        const entt::entity Spawned = World->SpawnParticleSystem(ParticleSystem, SpawnTransform, Lifetime);
        if (Spawned != entt::null)
        {
            World->GetComponent<SParticleSystemComponent>(Spawned).EmitterOffset = Offset;
        }
    }

    void SAnimNotify_Log::Notify(FEntityRegistry& Registry, FEntity Entity) const
    {
        LOG_INFO("AnimNotify '{}' on entity {}", Message, (uint32)entt::to_integral(Entity));
    }

    void SAnimNotifyState_Log::NotifyBegin(FEntityRegistry& Registry, FEntity Entity) const
    {
        LOG_INFO("AnimNotifyState '{}' BEGIN on entity {}", Message, (uint32)entt::to_integral(Entity));
    }

    void SAnimNotifyState_Log::NotifyTick(FEntityRegistry& Registry, FEntity Entity, float Alpha) const
    {
        if (bLogTick)
        {
            LOG_INFO("AnimNotifyState '{}' TICK {:.3f} on entity {}", Message, Alpha, (uint32)entt::to_integral(Entity));
        }
    }

    void SAnimNotifyState_Log::NotifyEnd(FEntityRegistry& Registry, FEntity Entity) const
    {
        LOG_INFO("AnimNotifyState '{}' END on entity {}", Message, (uint32)entt::to_integral(Entity));
    }
}
