#include "RuntimePCH.h"

#include "World/Subsystems/PerceptionSubsystem.h"

#include "AI/Perception/PerceptionWorldState.h"
#include "Physics/PhysicsScene.h"
#include "Physics/Ray/RayCast.h"
#include "World/Entity/Components/AIStimuliSourceComponent.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

namespace Lumina
{
    void CPerceptionSubsystem::GetPerceivedTargets(ECS::FEntity Perceiver,
        TVector<ECS::FEntity>& Out)
    {
        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            return;
        }

        const SPerceptionComponent* Perception = World->TryGetComponent<ECS::Read<SPerceptionComponent>>(Perceiver);
        if (Perception == nullptr)
        {
            return;
        }

        for (int32 Index = 0; Index < Perception->PerceivedCount; ++Index)
        {
            Out.Add(Perception->PerceivedTargets[Index].Target);
        }
    }

    bool CPerceptionSubsystem::CanSense(ECS::FEntity Perceiver, ECS::FEntity Target,
        EAISenseChannel Senses)
    {
        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            return false;
        }
        const SPerceptionComponent* Comp = World->TryGetComponent<ECS::Read<SPerceptionComponent>>(Perceiver);
        if (Comp == nullptr)
        {
            return false;
        }
        const int32 Index = Comp->Find(Target);
        return Index >= 0 && (Comp->PerceivedTargets[Index].ActiveSenses & (uint8)Senses) != 0;
    }

    SNavPoint CPerceptionSubsystem::GetLastKnownLocation(ECS::FEntity Perceiver,
        ECS::FEntity Target)
    {
        CWorld* World = GetWorld();
        SNavPoint Result;
        if (World == nullptr)
        {
            return Result;
        }
        const SPerceptionComponent* Comp = World->TryGetComponent<ECS::Read<SPerceptionComponent>>(Perceiver);
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

    ECS::FEntity CPerceptionSubsystem::GetClosestPerceivedTarget(ECS::FEntity Perceiver)
    {
        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            return ECS::NullEntity;
        }
        const SPerceptionComponent* Comp = World->TryGetComponent<ECS::Read<SPerceptionComponent>>(Perceiver);
        return Comp != nullptr ? Comp->GetClosestPerceivedTarget() : ECS::NullEntity;
    }

    bool CPerceptionSubsystem::HasLineOfSight(ECS::FEntity From, ECS::FEntity To)
    {
        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            return false;
        }

        Physics::IPhysicsScene* Scene = World->GetPhysicsScene();
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        const STransformComponent* FromXf = Registry.TryGet<ECS::Read<STransformComponent>>(From);
        const STransformComponent* ToXf = Registry.TryGet<ECS::Read<STransformComponent>>(To);
        if (Scene == nullptr || FromXf == nullptr || ToXf == nullptr)
        {
            return false;
        }

        FVector3 Start = FromXf->GetWorldLocation();
        FVector3 End = ToXf->GetWorldLocation();
        if (const SPerceptionComponent* Comp = Registry.TryGet<ECS::Read<SPerceptionComponent>>(From))
        {
            Start = Start + Comp->EyeOffset;
        }
        if (const SAIStimuliSourceComponent* Source = Registry.TryGet<ECS::Read<SAIStimuliSourceComponent>>(To))
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
            Ray.IgnoreEntities.Add(FromBody);
        }

        const TOptional<SRayResult> Hit = Scene->CastRay(Ray);
        return !Hit.IsSet() || Hit->Entity == To.Value;
    }

    void CPerceptionSubsystem::ReportNoise(FVector3 Location, float Loudness,
        ECS::FEntity Instigator)
    {
        CWorld* World = GetWorld();
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
        Perception::EnqueueStimulus(Perception::GetOrCreateState(World->GetEntityRegistry()), Event);
    }

    void CPerceptionSubsystem::ReportDamage(ECS::FEntity Victim, ECS::FEntity Instigator,
        FVector3 HitLocation, float Amount)
    {
        CWorld* World = GetWorld();
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
        Perception::EnqueueStimulus(Perception::GetOrCreateState(World->GetEntityRegistry()), Event);
    }

    void CPerceptionSubsystem::RegisterSource(ECS::FEntity Entity,
        const FName& AffiliationTag, EAISenseChannel Senses)
    {
        CWorld* World = GetWorld();
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
            SHierarchicalTag Tag;
            Tag.TagName = AffiliationTag;
            Source.AffiliationTags.AddTag(Tag);
        }
    }

    void CPerceptionSubsystem::AddDetectableTag(ECS::FEntity Perceiver, const FName& Tag)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || Tag.IsNone())
        {
            return;
        }
        SHierarchicalTag Detectable;
        Detectable.TagName = Tag;
        World->GetOrEmplaceComponent<SPerceptionComponent>(Perceiver).DetectableTags.AddTag(Detectable);
    }
}
