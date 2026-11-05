#include "RuntimePCH.h"
#include "PerceptionSystem.h"
#include "EntityScriptSystem.h"
#include "PathFollowSystem.h"

#include "Core/Console/ConsoleVariable.h"
#include "Core/Threading/Thread.h"
#include "Physics/PhysicsScene.h"
#include "Physics/Ray/RayCast.h"

namespace Lumina
{
    void SPerceptionSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        RunsBefore<SPathFollowSystem, SEntityScriptSystem>();
    }

    namespace PerceptionSystemInternal
    {
        TConsoleVar<bool> CVarPerceptionDebug("ai.Perception.Debug", false,
            "Draw AI perception sight cones, sense ranges, and lines to perceived targets.");

        constexpr uint8 SightBit      = (uint8)EAISenseChannel::Sight;
        constexpr uint8 HearingBit    = (uint8)EAISenseChannel::Hearing;
        constexpr uint8 DamageBit     = (uint8)EAISenseChannel::Damage;
        constexpr uint8 MomentaryBits = HearingBit | DamageBit; // cleared every tick after firing; only sight is sticky.

        // An empty perceiver filter senses everyone; otherwise the source must carry a matching tag.
        bool PassesAffiliation(const SHierarchicalTagContainer& Filter, const SHierarchicalTagContainer* SourceTags)
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
            else
            {
                int32 Slot = Comp.PerceivedCount;
                if (Slot >= SPerceptionComponent::MaxPerceivedTargets)
                {
                    // A full list gives up its stalest entry, or its weakest one, so a crowd of allies cannot hide a newcomer.
                    Slot = 0;
                    for (int32 i = 1; i < Comp.PerceivedCount; ++i)
                    {
                        const FPerceivedTarget& Candidate = Comp.PerceivedTargets[i];
                        const FPerceivedTarget& Current = Comp.PerceivedTargets[Slot];
                        if (Candidate.TimeSinceLastSensed > Current.TimeSinceLastSensed
                            || (Candidate.TimeSinceLastSensed == Current.TimeSinceLastSensed && Candidate.LastStrength < Current.LastStrength))
                        {
                            Slot = i;
                        }
                    }
                    const FPerceivedTarget& Victim = Comp.PerceivedTargets[Slot];
                    if (Victim.TimeSinceLastSensed <= 0.0f && Victim.LastStrength >= Strength)
                    {
                        return;
                    }
                }
                else
                {
                    ++Comp.PerceivedCount;
                }

                FPerceivedTarget& T = Comp.PerceivedTargets[Slot];
                T.Target = Target;
                T.LastKnownLocation = Location;
                T.ActiveSenses = SenseBit;
                T.TimeSinceLastSensed = 0.0f;
                T.LastStrength = Strength;
                T.bIsNew = true;
            }
        }

        // Apply one drained hearing/damage report to the relevant perceiver(s).
        template<typename TPerceiverQuery, typename TInstigatorQuery>
        void ApplyStimulus(const TPerceiverQuery& PerceiverQuery, const TInstigatorQuery& InstigatorQuery, const TVector<ECS::FEntity>& Perceivers, const FAIStimulusEvent& Stim)
        {
            const SHierarchicalTagContainer* SrcTags = nullptr;
            if (!Stim.Tags.IsEmpty())
            {
                SrcTags = &Stim.Tags;
            }
            else if (const SAIStimuliSourceComponent* Src = InstigatorQuery.template TryGet<SAIStimuliSourceComponent>(Stim.Instigator))
            {
                SrcTags = &Src->AffiliationTags;
            }

            if (Stim.Sense == EAISenseChannel::Damage)
            {
                // The victim perceives whoever damaged it, since you always notice an attacker.
                SPerceptionComponent* Comp = PerceiverQuery.template TryGet<SPerceptionComponent>(Stim.Target);
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
                SPerceptionComponent* Comp = PerceiverQuery.template TryGet<SPerceptionComponent>(E);
                if (Comp == nullptr || !Comp->bHearingEnabled)
                {
                    continue;
                }
                if (!PassesAffiliation(Comp->DetectableTags, SrcTags))
                {
                    continue;
                }
                const STransformComponent* Xf = PerceiverQuery.template TryGet<STransformComponent>(E);
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

        // Never inside the acquire shape, or a target between the two is taken and dropped on alternate scans.
        float LoseSightRange(const SPerceptionComponent& Comp)
        {
            return Math::Max(Comp.LoseSightRadius, Comp.SightRadius);
        }

        float LoseSightFOV(const SPerceptionComponent& Comp)
        {
            return Math::Max(Comp.LoseSightFOVDegrees, Comp.SightFOVDegrees);
        }

        SPerceptionEvent MakePerceptionEvent(ECS::FEntity Perceiver, const FPerceivedTarget& Target, bool bSensed)
        {
            SPerceptionEvent Event;
            Event.Perceiver  = Perceiver;
            Event.Target     = Target.Target;
            Event.Location   = Target.LastKnownLocation;
            Event.Sense      = (EAISenseChannel)Target.ActiveSenses;
            Event.Strength   = Target.LastStrength;
            Event.bPerceived = bSensed;
            return Event;
        }

        void DrawPerceptionDebug(const SSystemContext& Context, const SPerceptionComponent& Comp,
                                 const FVector3& Eye, const FVector3& Loc, const FVector3& Forward)
        {
            constexpr float OneFrame = -1.0f;
            if (Comp.bSightEnabled)
            {
                Context.DrawDebugCone(Eye, Forward, Math::Radians(Comp.SightFOVDegrees * 0.5f), Comp.SightRadius,
                    FVector4(0.2f, 0.8f, 1.0f, 1.0f), 16, 4, 1.0f, OneFrame);
                Context.DrawDebugCone(Eye, Forward, Math::Radians(LoseSightFOV(Comp) * 0.5f),
                    LoseSightRange(Comp), FVector4(0.25f, 0.3f, 0.6f, 1.0f), 16, 4, 1.0f, OneFrame);
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

    bool SPerceptionSystem::HasWork(const SSystemContext& Context)
    {
        return !Perceivers.IsEmpty();
    }

    void SPerceptionSystem::OnUpdate(const SSystemContext& Context)
    {
        using namespace PerceptionSystemInternal;

        const float DeltaTime = Context.GetDeltaTime();

        // The scheduler resolves dirty transforms before every batch, and the body never mutates one.

        FPerceptionWorldState& State = *PerceptionState;
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();

        State.SourceGrid.Sources.Reset();
        Sources.ForEach([&](ECS::FEntity Src, const SAIStimuliSourceComponent& S, const STransformComponent& Xf)
        {
            FPerceptionSource PS;
            PS.Entity = Src;
            PS.AimPoint = Xf.GetWorldLocationCached() + S.SightTargetOffset;
            PS.AffiliationTags = &S.AffiliationTags;
            PS.RegisteredSenses = (uint8)S.RegisteredSenses;
            State.SourceGrid.Sources.Add(PS);
        });

        float MaxSightRange = 1.0f;
        Perceivers.ForEach([&](const SPerceptionComponent& C, const STransformComponent&)
        {
            MaxSightRange = Math::Max(LoseSightRange(C), MaxSightRange);
        });
        State.SourceGrid.Build(MaxSightRange);

        Stimuli.Reset();
        {
            FScopeLock Lock(State.StimuliMutex);
            swap(Stimuli, State.PendingStimuli);
        }

        const bool bDebugAll = CVarPerceptionDebug.GetValue();
        const FPerceptionGrid& Grid = State.SourceGrid;

        // Read-only for the whole parallel body, so the workers share it without synchronizing.
        const FSignificanceState& Significance = *SignificanceState;
        Perceivers.ParallelForEach([&](ECS::FEntity E, SPerceptionComponent& Comp, const STransformComponent& Xform)
        {
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
            if (Comp.UpdateAccumulator < Comp.UpdateInterval * (float)Significance.Get(E).TickInterval)
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

            const float CosHalfFOV     = Math::Cos(Math::Radians(Comp.SightFOVDegrees * 0.5f));
            const float CosHalfLoseFOV = Math::Cos(Math::Radians(LoseSightFOV(Comp) * 0.5f));
            const float LoseRange      = LoseSightRange(Comp);
            const ECS::FEntity SelfBody = E;

            Grid.ForEachInRadius(Eye, LoseRange, [&](const FPerceptionSource& Src)
            {
                if (Src.Entity == E || (Src.RegisteredSenses & SightBit) == 0)
                {
                    return;
                }
                if (!PassesAffiliation(Comp.DetectableTags, Src.AffiliationTags))
                {
                    return;
                }

                // A seen target is held to the wider lose-sight cone and radius, so one on the edge of either does not flicker.
                const int32 Existing = Comp.Find(Src.Entity);
                const bool bAlreadySighted = (Existing >= 0 && WasSighted[Existing]);
                const float Range = bAlreadySighted ? LoseRange : Comp.SightRadius;
                const float CosHalfCone = bAlreadySighted ? CosHalfLoseFOV : CosHalfFOV;

                const FVector3 To = Src.AimPoint - Eye;
                const float Dist = Math::Length(To);
                if (Dist > Range)
                {
                    return;
                }
                if (Dist > 1e-4f)
                {
                    const FVector3 Dir = To / Dist;
                    const float Facing = Dir.x * Forward.x + Dir.y * Forward.y + Dir.z * Forward.z;
                    if (Facing < CosHalfCone)
                    {
                        return;
                    }
                }

                SRayCastSettings Ray;
                Ray.Start = Eye;
                Ray.End = Src.AimPoint;
                Ray.LayerMask = Comp.SightBlockingMask;
                Ray.IgnoreEntities.Add(SelfBody);
                const TOptional<SRayResult> Hit = Scene->CastRay(Ray);
                if (Hit.IsSet() && Hit->Entity != (uint32)Src.Entity)
                {
                    return;
                }

                // Nearer reads stronger, which decides who keeps a slot when more are in view than the list holds.
                MarkSensed(Comp, Src.Entity, SightBit, Src.AimPoint, 1.0f - Dist / Math::Max(Range, 1e-3f));
            });
        }, 1u);

        if (!Stimuli.IsEmpty())
        {
            const TVector<ECS::FEntity>& PerceiverList = Perceivers.Collect();
            for (const FAIStimulusEvent& Stim : Stimuli)
            {
                ApplyStimulus(Perceivers, Instigators, PerceiverList, Stim);
            }
            Perceivers.CompleteRun();
        }

        Perceivers.ForEach([&](ECS::FEntity E, SPerceptionComponent& Comp, const STransformComponent&)
        {
            for (int32 i = 0; i < Comp.PerceivedCount; )
            {
                FPerceivedTarget& T = Comp.PerceivedTargets[i];

                if (!Context.IsValidEntity(T.Target))
                {
                    FPerceivedTarget Lost = T;
                    Lost.ActiveSenses = 0;
                    RemoveAt(Comp, i);
                    PerceptionEvents.Send(MakePerceptionEvent(E, Lost, false));
                    continue;
                }

                if (T.bIsNew)
                {
                    T.bIsNew = false;
                    PerceptionEvents.Send(MakePerceptionEvent(E, T, true));
                }

                T.TimeSinceLastSensed = (T.ActiveSenses != 0) ? 0.0f : (T.TimeSinceLastSensed + DeltaTime);
                T.ActiveSenses &= ~MomentaryBits; // hearing/damage last only the tick of the report.

                if (T.TimeSinceLastSensed > Comp.ForgetTime)
                {
                    FPerceivedTarget Lost = T;
                    Lost.ActiveSenses = 0;
                    RemoveAt(Comp, i);
                    PerceptionEvents.Send(MakePerceptionEvent(E, Lost, false));
                    continue;
                }
                ++i;
            }
        });
    }
}
