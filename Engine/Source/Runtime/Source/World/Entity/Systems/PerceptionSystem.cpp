#include "Core/Threading/Thread.h"
#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "PerceptionSystem.h"

#include <algorithm>

#include "AI/Perception/PerceptionWorldState.h"
#include "Core/Console/ConsoleVariable.h"
#include "Physics/PhysicsScene.h"
#include "Physics/Ray/RayCast.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/AIStimuliSourceComponent.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Events/PerceptionEvent.h"
#include "World/Entity/Systems/SignificanceSystem.h"

namespace Lumina
{
    FSystemAccess SPerceptionSystem::Access = FSystemAccess::Exclusive();

    FOnPerceptionUpdated SPerceptionSystem::OnTargetPerceived;
    FOnPerceptionUpdated SPerceptionSystem::OnTargetLost;

    void SPerceptionSystem::Startup(const FSystemContext& Context) noexcept
    {
        // Created serially at world init so Update only ever READS the registry context.
        Perception::GetOrCreateState(Context.GetRegistry());
    }

    namespace
    {
        TConsoleVar<bool> CVarPerceptionDebug("ai.Perception.Debug", false,
            "Draw AI perception sight cones, sense ranges, and lines to perceived targets.");

        constexpr uint8 SightBit      = (uint8)EAISenseChannel::Sight;
        constexpr uint8 HearingBit    = (uint8)EAISenseChannel::Hearing;
        constexpr uint8 DamageBit     = (uint8)EAISenseChannel::Damage;
        constexpr uint8 MomentaryBits = HearingBit | DamageBit; // cleared every tick after firing; only sight is sticky.

        // Fired after the diff pass so a callback adding components cannot invalidate the walk.
        struct FPendingEvent
        {
            ECS::FEntity     Perceiver;
            FPerceivedTarget Target;
            bool             bSensed;
        };

        // An empty perceiver filter senses everyone; otherwise the source must carry a matching tag.
        bool PassesAffiliation(const FGameplayTagContainer& Filter, const FGameplayTagContainer* SourceTags)
        {
            if (Filter.IsEmpty())
            {
                return true;
            }
            return SourceTags != nullptr && Filter.HasAny(*SourceTags);
        }

        void RemoveAt(SPerceptionComponent& Comp, int32 Index)
        {
            Comp.PerceivedTargets[Index] = Comp.PerceivedTargets[Comp.PerceivedCount - 1];
            --Comp.PerceivedCount;
        }

        // Set/refresh a target on a perceiver via one sense. Adds a new record (flagged bIsNew) on first sight.
        void MarkSensed(SPerceptionComponent& Comp, ECS::FEntity Target, uint8 SenseBit, const FVector3& Location, float Strength)
        {
            if (Target == ECS::NullEntity)
            {
                return;
            }
            const int32 Index = Comp.Find(Target);
            if (Index >= 0)
            {
                FPerceivedTarget& T = Comp.PerceivedTargets[Index];
                T.ActiveSenses |= SenseBit;
                T.TimeSinceLastSensed = 0.0f;
                T.LastKnownLocation = Location;
                T.LastStrength = Strength;
            }
            else if (Comp.PerceivedCount < SPerceptionComponent::MaxPerceivedTargets)
            {
                FPerceivedTarget& T = Comp.PerceivedTargets[Comp.PerceivedCount++];
                T.Target = Target;
                T.LastKnownLocation = Location;
                T.ActiveSenses = SenseBit;
                T.TimeSinceLastSensed = 0.0f;
                T.LastStrength = Strength;
                T.bIsNew = true;
            }
        }

        // Apply one drained hearing/damage report to the relevant perceiver(s).
        void ApplyStimulus(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Perceivers, const FAIStimulusEvent& Stim)
        {
            const FGameplayTagContainer* SrcTags = nullptr;
            if (!Stim.Tags.IsEmpty())
            {
                SrcTags = &Stim.Tags;
            }
            else if (const SAIStimuliSourceComponent* Src = Registry.TryGet<SAIStimuliSourceComponent>(Stim.Instigator))
            {
                SrcTags = &Src->AffiliationTags;
            }

            if (Stim.Sense == EAISenseChannel::Damage)
            {
                // The victim perceives whoever damaged it, since you always notice an attacker.
                SPerceptionComponent* Comp = Registry.TryGet<SPerceptionComponent>(Stim.Target);
                if (Comp != nullptr && Comp->bDamageEnabled)
                {
                    MarkSensed(*Comp, Stim.Instigator, DamageBit, Stim.Location, Stim.Strength);
                }
                return;
            }

            // Hearing reaches every in-range perceiver that cares about the instigator's affiliation.
            for (ECS::FEntity E : Perceivers)
            {
                if (E == Stim.Instigator)
                {
                    continue;
                }
                SPerceptionComponent* Comp = Registry.TryGet<SPerceptionComponent>(E);
                if (Comp == nullptr || !Comp->bHearingEnabled)
                {
                    continue;
                }
                if (!PassesAffiliation(Comp->DetectableTags, SrcTags))
                {
                    continue;
                }
                const STransformComponent* Xf = Registry.TryGet<STransformComponent>(E);
                if (Xf == nullptr)
                {
                    continue;
                }
                const FVector3 Ear = Xf->GetWorldLocationCached() + Comp->EyeOffset;
                const float Radius = Comp->HearingRadius * (Stim.Strength > 0.0f ? Stim.Strength : 1.0f);
                if (Math::Length(Stim.Location - Ear) > Radius)
                {
                    continue;
                }
                MarkSensed(*Comp, Stim.Instigator, HearingBit, Stim.Location, Stim.Strength);
            }
        }

        void FirePerceptionEvent(const FSystemContext& Context, ECS::FEntity Perceiver, const FPerceivedTarget& Target, bool bSensed)
        {
            if (bSensed)
            {
                SPerceptionSystem::OnTargetPerceived.Broadcast(Perceiver, Target);
            }
            else
            {
                SPerceptionSystem::OnTargetLost.Broadcast(Perceiver, Target);
            }

            Context.DispatchEvent<FPerceptionUpdatedEvent>(FPerceptionUpdatedEvent{
                Perceiver, Target.Target, (EAISenseChannel)Target.ActiveSenses, Target.LastKnownLocation, bSensed });

            SPerceptionComponent* Comp = Context.TryGet<SPerceptionComponent>(Perceiver);
            if (Comp == nullptr)
            {
                return;
            }
            TScriptDelegate<SPerceptionEvent>& Delegate = bSensed ? Comp->OnTargetPerceived : Comp->OnTargetLost;

            // A script override must fire whether or not anything bound the component's delegate.
            ECS::FRegistry& Registry = Context.GetRegistry();
            const bool bHasScripts = Registry.HasAll<SEntityScriptComponent>(Perceiver);
            if (!Delegate.IsBound() && !bHasScripts)
            {
                return;
            }

            SPerceptionEvent Payload;
            Payload.Perceiver = Perceiver;
            Payload.Target    = Target.Target;
            Payload.Location  = Target.LastKnownLocation;
            Payload.Sense     = (EAISenseChannel)Target.ActiveSenses;
            Payload.Strength  = Target.LastStrength;

            if (Delegate.IsBound())
            {
                Delegate.Broadcast(Payload);
            }
            if (bHasScripts)
            {
                EntityScripts::DispatchPerception(Registry, Perceiver, bSensed, Payload);
            }
        }

        void DrawPerceptionDebug(const FSystemContext& Context, const SPerceptionComponent& Comp,
                                 const FVector3& Eye, const FVector3& Loc, const FVector3& Forward)
        {
            constexpr float OneFrame = -1.0f;
            if (Comp.bSightEnabled)
            {
                Context.DrawDebugCone(Eye, Forward, Math::Radians(Comp.SightFOVDegrees * 0.5f), Comp.SightRadius,
                    FVector4(0.2f, 0.8f, 1.0f, 1.0f), 16, 4, 1.0f, OneFrame);
                Context.DrawDebugSphere(Eye, Comp.LoseSightRadius, FVector4(0.25f, 0.3f, 0.6f, 1.0f), 16, 1.0f, OneFrame);
            }
            if (Comp.bHearingEnabled)
            {
                Context.DrawDebugSphere(Loc, Comp.HearingRadius, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 16, 1.0f, OneFrame);
            }
            for (int32 i = 0; i < Comp.PerceivedCount; ++i)
            {
                const FPerceivedTarget& T = Comp.PerceivedTargets[i];
                const FVector4 Color = T.ActiveSenses != 0
                    ? FVector4(0.1f, 1.0f, 0.1f, 1.0f)   // currently sensed
                    : FVector4(1.0f, 0.6f, 0.0f, 1.0f);  // remembered (last known)
                Context.DrawDebugLine(Eye, T.LastKnownLocation, Color, 2.0f, OneFrame);
            }
        }
    }

    void SPerceptionSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        const float DeltaTime = (float)Context.GetDeltaTime();
        auto View = Context.CreateView<SPerceptionComponent>();
        auto Handle = View.GetDriver();
        if (Handle->IsEmpty())
        {
            return;
        }
        
        // Worker-visible storage, since a lambda never captures a thread_local and workers index this.
        TVector<ECS::FEntity> Perceivers;
        Perceivers.reserve(Handle->GetDenseSize());
        for (ECS::FEntity E : *Handle)
        {
            Perceivers.push_back(E);
        }
        const int32 NumPerceivers = (int32)Perceivers.size();

        // Bulk-resolve before the parallel body; the body never mutates a transform.
        ECS::Utils::ResolveAllDirtyTransforms(Context.GetRegistry());

        ECS::FRegistry& Registry = Context.GetRegistry();
        
        FPerceptionWorldState* StatePtr = Registry.Ctx().Find<FPerceptionWorldState>();
        if (StatePtr == nullptr)
        {
            return;
        }
        FPerceptionWorldState& State = *StatePtr;

        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        Physics::IPhysicsScene* Scene = Context.GetPhysicsScene();
        
        State.SourceGrid.Sources.clear();
        {
            auto SourceView = Context.CreateView<SAIStimuliSourceComponent, STransformComponent>();
            for (ECS::FEntity Src : SourceView)
            {
                const SAIStimuliSourceComponent& S = SourceView.Get<SAIStimuliSourceComponent>(Src);
                const STransformComponent& Xf = SourceView.Get<STransformComponent>(Src);
                FPerceptionSource PS;
                PS.Entity = Src;
                PS.AimPoint = Xf.GetWorldLocationCached() + S.SightTargetOffset;
                PS.AffiliationTags = &S.AffiliationTags;
                PS.BodyID = Context.GetEntityBodyID(Src);
                PS.RegisteredSenses = (uint8)S.RegisteredSenses;
                State.SourceGrid.Sources.push_back(PS);
            }
        }

        TVector<uint32> PerceiverBodies((size_t)NumPerceivers);
        float MaxSightRange = 1.0f;
        for (int32 i = 0; i < NumPerceivers; ++i)
        {
            PerceiverBodies[i] = Context.GetEntityBodyID(Perceivers[i]);
            const SPerceptionComponent& C = View.Get<SPerceptionComponent>(Perceivers[i]);
            MaxSightRange = Math::Max(C.LoseSightRadius, MaxSightRange);
        }
        State.SourceGrid.Build(MaxSightRange);

        TVector<FAIStimulusEvent> Stimuli;
        {
            FScopeLock Lock(State.StimuliMutex);
            Stimuli.swap(State.PendingStimuli);
        }

        const bool bDebugAll = CVarPerceptionDebug.GetValue();
        const FPerceptionGrid& Grid = State.SourceGrid;

        // Read-only for the whole parallel body, so the workers share one pointer without synchronizing.
        const FSignificanceState* SignificanceState = Significance::GetState(Context);
        Task::ParallelFor((uint32)NumPerceivers, [&](uint32 Index)
        {
            const ECS::FEntity E = Perceivers[Index];
            SPerceptionComponent& Comp = View.Get<SPerceptionComponent>(E);
            const STransformComponent& Xform = TransformStorage.Get(E);

            const FVector3 Loc = Xform.GetWorldLocationCached();
            const FVector3 Eye = Loc + Comp.EyeOffset;
            const FVector3 Forward = Xform.GetWorldTransformCached().GetForward();
            Comp.LastEyeLocation = Eye;

            if (bDebugAll || Comp.bDrawDebug)
            {
                DrawPerceptionDebug(Context, Comp, Eye, Loc, Forward);
            }

            Comp.UpdateAccumulator += DeltaTime;
            if (!Comp.bSightEnabled || Scene == nullptr)
            {
                return;
            }
            if (Comp.UpdateAccumulator < Significance::ScaleInterval(SignificanceState, E, Comp.UpdateInterval))
            {
                return;
            }
            Comp.UpdateAccumulator = 0.0f;
            
            bool WasSighted[SPerceptionComponent::MaxPerceivedTargets];
            for (int32 i = 0; i < Comp.PerceivedCount; ++i)
            {
                WasSighted[i] = (Comp.PerceivedTargets[i].ActiveSenses & SightBit) != 0;
                Comp.PerceivedTargets[i].ActiveSenses &= ~SightBit;
            }

            const float CosHalfFOV = Math::Cos(Math::Radians(Comp.SightFOVDegrees * 0.5f));
            const uint32 SelfBody = PerceiverBodies[Index];

            Grid.ForEachInRadius(Eye, Comp.LoseSightRadius, [&](const FPerceptionSource& Src)
            {
                if (Src.Entity == E || (Src.RegisteredSenses & SightBit) == 0)
                {
                    return;
                }
                if (!PassesAffiliation(Comp.DetectableTags, Src.AffiliationTags))
                {
                    return;
                }

                // The FOV cone gates fresh acquisition only, or a target stepping aside is dropped each scan.
                const int32 Existing = Comp.Find(Src.Entity);
                const bool bAlreadySighted = (Existing >= 0 && WasSighted[Existing]);
                const float Range = bAlreadySighted ? Comp.LoseSightRadius : Comp.SightRadius;

                const FVector3 To = Src.AimPoint - Eye;
                const float Dist = Math::Length(To);
                if (Dist > Range)
                {
                    return;
                }
                if (!bAlreadySighted && Dist > 1e-4f)
                {
                    const FVector3 Dir = To / Dist;
                    const float Facing = Dir.x * Forward.x + Dir.y * Forward.y + Dir.z * Forward.z;
                    if (Facing < CosHalfFOV)
                    {
                        return;
                    }
                }

                SRayCastSettings Ray;
                Ray.Start = Eye;
                Ray.End = Src.AimPoint;
                Ray.LayerMask = Comp.SightBlockingMask;
                Ray.IgnoreBodies.push_back(SelfBody);
                const TOptional<SRayResult> Hit = Scene->CastRay(Ray);
                if (Hit.has_value() && Hit->Entity != (uint32)Src.Entity)
                {
                    return;
                }

                MarkSensed(Comp, Src.Entity, SightBit, Src.AimPoint, 0.0f);
            });
        });

        for (const FAIStimulusEvent& Stim : Stimuli)
        {
            ApplyStimulus(Registry, Perceivers, Stim);
        }
        
        TVector<FPendingEvent> Events;
        for (ECS::FEntity E : Perceivers)
        {
            SPerceptionComponent* Comp = Registry.TryGet<SPerceptionComponent>(E);
            if (Comp == nullptr)
            {
                continue;
            }

            for (int32 i = 0; i < Comp->PerceivedCount; )
            {
                FPerceivedTarget& T = Comp->PerceivedTargets[i];

                if (!Context.IsValidEntity(T.Target))
                {
                    FPerceivedTarget Lost = T;
                    Lost.ActiveSenses = 0;
                    RemoveAt(*Comp, i);
                    Events.push_back({ E, Lost, false });
                    continue;
                }

                if (T.bIsNew)
                {
                    T.bIsNew = false;
                    Events.push_back({ E, T, true });
                }

                T.TimeSinceLastSensed = (T.ActiveSenses != 0) ? 0.0f : (T.TimeSinceLastSensed + DeltaTime);
                T.ActiveSenses &= ~MomentaryBits; // hearing/damage last only the tick of the report.

                if (T.TimeSinceLastSensed > Comp->ForgetTime)
                {
                    FPerceivedTarget Lost = T;
                    Lost.ActiveSenses = 0;
                    RemoveAt(*Comp, i);
                    Events.push_back({ E, Lost, false });
                    continue;
                }
                ++i;
            }
        }

        for (const FPendingEvent& Ev : Events)
        {
            FirePerceptionEvent(Context, Ev.Perceiver, Ev.Target, Ev.bSensed);
        }
    }
}
