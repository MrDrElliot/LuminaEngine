#include <algorithm>

#include "Platform/GenericPlatform.h"
#include "Scripting/DotNet/LayoutRegistry.h"
#include "Containers/String.h"
#include "Containers/Name.h"
#include "Core/Math/Math.h"
#include "Core/Templates/Optional.h"
#include "World/World.h"
#include "Physics/PhysicsScene.h"
#include "Physics/PhysicsTypes.h"
#include "Physics/Ray/RayCast.h"
#include "GameplayTags/GameplayTag.h"
#include "GameplayTags/GameplayTagRegistry.h"
#include "AI/Perception/PerceptionTypes.h"
#include "AI/Perception/PerceptionWorldState.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/AIStimuliSourceComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "Scripting/DotNet/DotNetExport.h"

//================================================================================================
// World.Perception: AI perception queries + event injection (LuminaSharp.Perception). Every export takes
// the opaque CWorld* (uint64) first; entities are entt ids (uint32). Game thread only. With no perceiver
// component present each query simply reports "nothing" (never throws). See SPerceptionSystem for the model.
//================================================================================================

using namespace Lumina;
using namespace Lumina::DotNet;

namespace
{
    // Blittable point result mirrored by LuminaSharp.PerceptionPointWire. bFound == 0 means unknown target.
    struct FLmPerceptionPoint
    {
        int32    bFound;
        FVector3 Point;
    };
}
LE_REGISTER_LAYOUT("PerceptionPointWire", FLmPerceptionPoint);

// Fills OutEntities (entt ids) with the perceiver's currently-tracked targets; returns the count (<= Max).
LUMINA_DOTNET_EXPORT(int32, Perception_GetPerceivedTargets)(uint64 World, uint32 Perceiver, uint32* OutEntities, int32 Max)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || OutEntities == nullptr || Max <= 0)
    {
        return 0;
    }
    const SPerceptionComponent* C = W->TryGetComponent<SPerceptionComponent>(AsEntity(Perceiver));
    if (C == nullptr)
    {
        return 0;
    }
    int32 Count = C->PerceivedCount;
    Count = std::min(Count, Max);
    for (int32 i = 0; i < Count; ++i)
    {
        OutEntities[i] = ToId(C->PerceivedTargets[i].Target);
    }
    return Count;
}

// 1 if Target is currently sensed by any of the SenseBits (EAISenseChannel) channels.
LUMINA_DOTNET_EXPORT(int32, Perception_CanSense)(uint64 World, uint32 Perceiver, uint32 Target, int32 SenseBits)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0;
    }
    const SPerceptionComponent* C = W->TryGetComponent<SPerceptionComponent>(AsEntity(Perceiver));
    if (C == nullptr)
    {
        return 0;
    }
    const int32 Index = C->Find(AsEntity(Target));
    return (Index >= 0 && (C->PerceivedTargets[Index].ActiveSenses & (uint8)SenseBits) != 0) ? 1 : 0;
}

// Last known location of Target (held during the forget window), or bFound == 0 if not tracked.
LUMINA_DOTNET_EXPORT(FLmPerceptionPoint, Perception_GetLastKnownLocation)(uint64 World, uint32 Perceiver, uint32 Target)
{
    FLmPerceptionPoint Result{};
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return Result;
    }
    const SPerceptionComponent* C = W->TryGetComponent<SPerceptionComponent>(AsEntity(Perceiver));
    if (C == nullptr)
    {
        return Result;
    }
    const int32 Index = C->Find(AsEntity(Target));
    if (Index >= 0)
    {
        Result.bFound = 1;
        Result.Point = C->PerceivedTargets[Index].LastKnownLocation;
    }
    return Result;
}

// Nearest perceived target to the perceiver's eye, or entt::null (0xFFFFFFFF) when nothing is perceived.
LUMINA_DOTNET_EXPORT(uint32, Perception_GetClosest)(uint64 World, uint32 Perceiver)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return ToId(entt::null);
    }
    const SPerceptionComponent* C = W->TryGetComponent<SPerceptionComponent>(AsEntity(Perceiver));
    return C != nullptr ? ToId(C->GetClosestPerceivedTarget()) : ToId(entt::null);
}

// One-shot line-of-sight test between two entities (eye/aim offsets applied when the components exist).
LUMINA_DOTNET_EXPORT(int32, Perception_HasLineOfSight)(uint64 World, uint32 FromEntity, uint32 ToEntity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0;
    }
    Physics::IPhysicsScene* Scene = W->GetPhysicsScene();
    auto& Registry = ECS::GetWorldRegistry(*W);
    const entt::entity From = AsEntity(FromEntity);
    const entt::entity To = AsEntity(ToEntity);
    const STransformComponent* FromXf = Registry.try_get<STransformComponent>(From);
    const STransformComponent* ToXf = Registry.try_get<STransformComponent>(To);
    if (Scene == nullptr || FromXf == nullptr || ToXf == nullptr)
    {
        return 0;
    }

    FVector3 Start = FromXf->GetWorldLocation();
    FVector3 End = ToXf->GetWorldLocation();
    if (const SPerceptionComponent* PC = Registry.try_get<SPerceptionComponent>(From))
    {
        Start = Start + PC->EyeOffset;
    }
    if (const SAIStimuliSourceComponent* SC = Registry.try_get<SAIStimuliSourceComponent>(To))
    {
        End = End + SC->SightTargetOffset;
    }

    SRayCastSettings Ray;
    Ray.Start = Start;
    Ray.End = End;
    Ray.LayerMask = Lumina::ECollisionProfiles::Static | Lumina::ECollisionProfiles::Dynamic;
    const uint32 FromBody = Scene->GetEntityBodyID(From);
    if (FromBody != 0xFFFFFFFFu)
    {
        Ray.IgnoreBodies.push_back(FromBody);
    }
    const TOptional<SRayResult> Hit = Scene->CastRay(Ray);
    return (!Hit.has_value() || Hit->Entity == ToId(To)) ? 1 : 0;
}

// Queue a noise report: heard by every perceiver within HearingRadius * Loudness that detects the instigator's affiliation.
LUMINA_DOTNET_EXPORT(void, Perception_ReportNoise)(uint64 World, FVector3 Location, float Loudness, uint32 Instigator)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    FAIStimulusEvent Event;
    Event.Sense = Lumina::EAISenseChannel::Hearing;
    Event.Instigator = AsEntity(Instigator);
    Event.Target = entt::null;
    Event.Location = Location;
    Event.Strength = Loudness;
    Perception::EnqueueStimulus(Perception::GetOrCreateState(ECS::GetWorldRegistry(*W)), Event);
}

// Queue a damage report: the victim immediately perceives its attacker (affiliation-agnostic).
LUMINA_DOTNET_EXPORT(void, Perception_ReportDamage)(uint64 World, uint32 Victim, uint32 Instigator, FVector3 HitLocation, float Amount)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    FAIStimulusEvent Event;
    Event.Sense = Lumina::EAISenseChannel::Damage;
    Event.Instigator = AsEntity(Instigator);
    Event.Target = AsEntity(Victim);
    Event.Location = HitLocation;
    Event.Strength = Amount;
    Perception::EnqueueStimulus(Perception::GetOrCreateState(ECS::GetWorldRegistry(*W)), Event);
}

// Make an entity perceivable: ensure an SAIStimuliSourceComponent, set its registered senses, add one
// affiliation tag (0 = none). Authoring richer sets is done in the editor.
LUMINA_DOTNET_EXPORT(void, Perception_RegisterSource)(uint64 World, uint32 Entity, uint32 AffiliationTagId, int32 SenseBits)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    SAIStimuliSourceComponent& Source = W->GetOrEmplaceComponent<SAIStimuliSourceComponent>(AsEntity(Entity));
    if (SenseBits != 0)
    {
        Source.RegisteredSenses = (Lumina::EAISenseChannel)(uint8)SenseBits;
    }
    if (AffiliationTagId != 0)
    {
        const FString Name = FGameplayTagRegistry::Get().GetName(AffiliationTagId);
        if (!Name.empty())
        {
            FGameplayTag Tag;
            Tag.TagName = FName(Name.c_str());
            Source.AffiliationTags.AddTag(Tag);
        }
    }
}

// Ensure an SPerceptionComponent on the perceiver and add one tag to its DetectableTags affiliation filter
// (only sources carrying a matching tag are then sensed; an empty filter senses everyone).
LUMINA_DOTNET_EXPORT(void, Perception_AddDetectableTag)(uint64 World, uint32 Perceiver, uint32 TagId)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || TagId == 0)
    {
        return;
    }
    const FString Name = FGameplayTagRegistry::Get().GetName(TagId);
    if (Name.empty())
    {
        return;
    }
    SPerceptionComponent& Comp = W->GetOrEmplaceComponent<SPerceptionComponent>(AsEntity(Perceiver));
    FGameplayTag Tag;
    Tag.TagName = FName(Name.c_str());
    Comp.DetectableTags.AddTag(Tag);
}
