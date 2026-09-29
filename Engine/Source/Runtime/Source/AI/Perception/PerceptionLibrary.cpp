#include "RuntimePCH.h"

#include "PerceptionLibrary.h"

#include "AI/Perception/PerceptionWorldState.h"
#include "Physics/PhysicsScene.h"
#include "Physics/Ray/RayCast.h"
#include "World/Entity/Components/AIStimuliSourceComponent.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

namespace Lumina
{
    void CPerceptionLibrary::GetPerceivedTargets(CWorld* World, ECS::FEntity Perceiver,
        TVector<ECS::FEntity>& Out)
    {
        if (World == nullptr)
        {
            return;
        }

        const SPerceptionComponent* Perception = World->TryGetComponent<SPerceptionComponent>(Perceiver);
        if (Perception == nullptr)
        {
            return;
        }

        for (int32 Index = 0; Index < Perception->PerceivedCount; ++Index)
        {
            Out.push_back(Perception->PerceivedTargets[Index].Target);
        }
    }

    bool CPerceptionLibrary::CanSense(CWorld* World, ECS::FEntity Perceiver, ECS::FEntity Target,
        EAISenseChannel Senses)
    {
        if (World == nullptr)
        {
            return false;
        }
        const SPerceptionComponent* Comp = World->TryGetComponent<SPerceptionComponent>(Perceiver);
        if (Comp == nullptr)
        {
            return false;
        }
        const int32 Index = Comp->Find(Target);
        return Index >= 0 && (Comp->PerceivedTargets[Index].ActiveSenses & (uint8)Senses) != 0;
    }

    FNavPoint CPerceptionLibrary::GetLastKnownLocation(CWorld* World, ECS::FEntity Perceiver,
        ECS::FEntity Target)
    {
        FNavPoint Result;
        if (World == nullptr)
        {
            return Result;
        }
        const SPerceptionComponent* Comp = World->TryGetComponent<SPerceptionComponent>(Perceiver);
        if (Comp == nullptr)
        {
            return Result;
        }
        const int32 Index = Comp->Find(Target);
        if (Index >= 0)
        {
            Result.bFound = true;
            Result.Point = Comp->PerceivedTargets[Index].LastKnownLocation;
        }
        return Result;
    }

    ECS::FEntity CPerceptionLibrary::GetClosestPerceivedTarget(CWorld* World, ECS::FEntity Perceiver)
    {
        if (World == nullptr)
        {
            return ECS::NullEntity;
        }
        const SPerceptionComponent* Comp = World->TryGetComponent<SPerceptionComponent>(Perceiver);
        return Comp != nullptr ? Comp->GetClosestPerceivedTarget() : ECS::NullEntity;
    }

    bool CPerceptionLibrary::HasLineOfSight(CWorld* World, ECS::FEntity From, ECS::FEntity To)
    {
        if (World == nullptr)
        {
            return false;
        }

        Physics::IPhysicsScene* Scene = World->GetPhysicsScene();
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        const STransformComponent* FromXf = Registry.TryGet<STransformComponent>(From);
        const STransformComponent* ToXf = Registry.TryGet<STransformComponent>(To);
        if (Scene == nullptr || FromXf == nullptr || ToXf == nullptr)
        {
            return false;
        }

        FVector3 Start = FromXf->GetWorldLocation();
        FVector3 End = ToXf->GetWorldLocation();
        if (const SPerceptionComponent* Comp = Registry.TryGet<SPerceptionComponent>(From))
        {
            Start = Start + Comp->EyeOffset;
        }
        if (const SAIStimuliSourceComponent* Source = Registry.TryGet<SAIStimuliSourceComponent>(To))
        {
            End = End + Source->SightTargetOffset;
        }

        SRayCastSettings Ray;
        Ray.Start = Start;
        Ray.End = End;
        Ray.LayerMask = ECollisionProfiles::Static | ECollisionProfiles::Dynamic;
        const ECS::FEntity FromBody = From;
        if (FromBody != ECS::NullEntity)
        {
            Ray.IgnoreEntities.push_back(FromBody);
        }

        const TOptional<SRayResult> Hit = Scene->CastRay(Ray);
        return !Hit.has_value() || Hit->Entity == To.Value;
    }

    void CPerceptionLibrary::ReportNoise(CWorld* World, FVector3 Location, float Loudness,
        ECS::FEntity Instigator)
    {
        if (World == nullptr)
        {
            return;
        }
        FAIStimulusEvent Event;
        Event.Sense = EAISenseChannel::Hearing;
        Event.Instigator = Instigator;
        Event.Target = ECS::NullEntity;
        Event.Location = Location;
        Event.Strength = Loudness;
        Perception::EnqueueStimulus(Perception::GetOrCreateState(ECS::GetWorldRegistry(*World)), Event);
    }

    void CPerceptionLibrary::ReportDamage(CWorld* World, ECS::FEntity Victim, ECS::FEntity Instigator,
        FVector3 HitLocation, float Amount)
    {
        if (World == nullptr)
        {
            return;
        }
        FAIStimulusEvent Event;
        Event.Sense = EAISenseChannel::Damage;
        Event.Instigator = Instigator;
        Event.Target = Victim;
        Event.Location = HitLocation;
        Event.Strength = Amount;
        Perception::EnqueueStimulus(Perception::GetOrCreateState(ECS::GetWorldRegistry(*World)), Event);
    }

    void CPerceptionLibrary::RegisterSource(CWorld* World, ECS::FEntity Entity,
        const FName& AffiliationTag, EAISenseChannel Senses)
    {
        if (World == nullptr)
        {
            return;
        }
        SAIStimuliSourceComponent& Source = World->GetOrEmplaceComponent<SAIStimuliSourceComponent>(Entity);
        if ((uint8)Senses != 0)
        {
            Source.RegisteredSenses = Senses;
        }
        if (!AffiliationTag.IsNone())
        {
            FGameplayTag Tag;
            Tag.TagName = AffiliationTag;
            Source.AffiliationTags.AddTag(Tag);
        }
    }

    void CPerceptionLibrary::AddDetectableTag(CWorld* World, ECS::FEntity Perceiver, const FName& Tag)
    {
        if (World == nullptr || Tag.IsNone())
        {
            return;
        }
        FGameplayTag Detectable;
        Detectable.TagName = Tag;
        World->GetOrEmplaceComponent<SPerceptionComponent>(Perceiver).DetectableTags.AddTag(Detectable);
    }
}
