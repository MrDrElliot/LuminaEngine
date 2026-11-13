#include "RuntimePCH.h"
#include "TweenManager.h"

#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Containers/Optional.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Memory/SmartPtr.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    namespace TweenManagerInternal
    {
        // A zero-length tweener still has to run its final value once, so it lands on the target.
        constexpr float kMinDuration = 1e-6f;

        // A pass shorter than this never carries leftover time into the next, so a zero-length loop runs once per tick.
        constexpr float kMinCarriedPass = 1e-4f;

        // Bounds how many short passes one long frame can run.
        constexpr int32 kMaxPassesPerTick = 64;

        NODISCARD uint32 HashSeed(uint32 Value)
        {
            Value ^= Value >> 16;
            Value *= 0x7feb352du;
            Value ^= Value >> 15;
            Value *= 0x846ca68bu;
            Value ^= Value >> 16;
            return Value;
        }

        NODISCARD float UnitFromSeed(uint32 Seed, uint32 Lane)
        {
            return (float)(HashSeed(Seed * 8u + Lane) & 0xFFFFFFu) / (float)0xFFFFFFu;
        }

        NODISCARD STransformComponent* FindTransform(ECS::FRegistry* Registry, ECS::FEntity Entity)
        {
            return Registry->IsValid(Entity) ? Registry->TryGet<ECS::Write<STransformComponent>>(Entity) : nullptr;
        }

        NODISCARD FQuat QuatFromDegrees(const FVector3& Degrees)
        {
            return FQuat(Math::Radians(Degrees));
        }

        // A dotted path through nested struct properties, walked again on every write since storage can move.
        struct FResolvedProperty
        {
            TVector<const FProperty*> Chain;
            ECS::FComponentTypeID     TypeID = ECS::InvalidComponentTypeID;
            uint64                    Generation = 0;

            NODISCARD const FProperty* Leaf() const { return Chain.Last(); }

            NODISCARD void* Address(void* Component) const
            {
                uint8* Value = static_cast<uint8*>(Component);
                for (const FProperty* Property : Chain)
                {
                    Value = Property->GetValuePtr<uint8>(Value);
                }
                return Value;
            }
        };

        bool ResolvePropertyPath(const CStruct* Component, FStringView Path, FResolvedProperty& Out)
        {
            const FComponentOps* Ops = Component != nullptr ? Component->GetComponentOps() : nullptr;
            if (Ops == nullptr || Path.IsEmpty())
            {
                return false;
            }
            Out.TypeID = Ops->GetTypeID();
            Out.Generation = FScriptStructRegistry::Get().GetGeneration();

            const CStruct* Owner = Component;
            int32 SegmentStart = 0;
            while (SegmentStart <= Path.Len())
            {
                int32 SegmentEnd = SegmentStart;
                while (SegmentEnd < Path.Len() && Path[SegmentEnd] != '.')
                {
                    ++SegmentEnd;
                }

                const FProperty* Property = Owner != nullptr ? Owner->GetProperty(FName(Path.Mid(SegmentStart, SegmentEnd - SegmentStart))) : nullptr;
                if (Property == nullptr)
                {
                    return false;
                }
                Out.Chain.Add(Property);

                if (SegmentEnd >= Path.Len())
                {
                    return true;
                }
                if (Property->GetType() != EPropertyTypeFlags::Struct)
                {
                    return false;
                }
                Owner = static_cast<const FStructProperty*>(Property)->GetStruct();
                SegmentStart = SegmentEnd + 1;
            }
            return false;
        }

        NODISCARD bool IsStructNamed(const FProperty* Property, const char* Name)
        {
            if (Property->GetType() != EPropertyTypeFlags::Struct)
            {
                return false;
            }
            const CStruct* Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
            return Struct != nullptr && Struct->GetName() == FName(Name);
        }

        // How one value type is read from and written to a reflected property it can drive.
        template <typename T>
        struct TPropertyValue
        {
            static bool Accepts(const FProperty* Property);
            static T    Read(const FProperty* Property, const void* Address) { return *static_cast<const T*>(Address); }
            static void Write(const FProperty* Property, void* Address, const T& Value) { *static_cast<T*>(Address) = Value; }
        };

        template <>
        struct TPropertyValue<float>
        {
            static bool Accepts(const FProperty* Property)
            {
                const EPropertyTypeFlags Kind = Property->GetType();
                return Kind == EPropertyTypeFlags::Float || Kind == EPropertyTypeFlags::Double || Kind == EPropertyTypeFlags::Int32;
            }

            static float Read(const FProperty* Property, const void* Address)
            {
                switch (Property->GetType())
                {
                case EPropertyTypeFlags::Double: return (float)*static_cast<const double*>(Address);
                case EPropertyTypeFlags::Int32:  return (float)*static_cast<const int32*>(Address);
                default:                         return *static_cast<const float*>(Address);
                }
            }

            static void Write(const FProperty* Property, void* Address, float Value)
            {
                switch (Property->GetType())
                {
                case EPropertyTypeFlags::Double: *static_cast<double*>(Address) = Value; break;
                case EPropertyTypeFlags::Int32:  *static_cast<int32*>(Address) = (int32)Math::Round(Value); break;
                default:                         *static_cast<float*>(Address) = Value; break;
                }
            }
        };

        template <> bool TPropertyValue<FVector2>::Accepts(const FProperty* Property) { return IsStructNamed(Property, "FVector2"); }
        template <> bool TPropertyValue<FVector3>::Accepts(const FProperty* Property) { return IsStructNamed(Property, "FVector3"); }
        template <> bool TPropertyValue<FVector4>::Accepts(const FProperty* Property) { return IsStructNamed(Property, "FVector4"); }
        template <> bool TPropertyValue<FQuat>::Accepts(const FProperty* Property)    { return IsStructNamed(Property, "FQuat"); }
    }

    FTweenManager::~FTweenManager()
    {
        Clear();
    }

    FTween FTweenManager::Create()
    {
        return CreateForEntity(ECS::NullEntity);
    }

    FTween FTweenManager::CreateForEntity(ECS::FEntity Owner)
    {
        const ECS::FEntity Entity = Registry.Create();
        FTweenState& State = Registry.Emplace<FTweenState>(Entity);
        State.Owner = Owner;

        FTweenHandle Handle;
        Handle.Handle = Entity;
        return FTween(this, Handle);
    }

    FTweenManager::FTweenState* FTweenManager::Find(FTweenHandle Handle)
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return nullptr;
        }
        return Registry.TryGet<ECS::Write<FTweenState>>(Handle.Handle);
    }

    const FTweenManager::FTweenState* FTweenManager::Find(FTweenHandle Handle) const
    {
        return const_cast<FTweenManager*>(this)->Find(Handle);
    }

    void FTweenManager::Kill(FTweenHandle Handle)
    {
        FTweenState* State = Find(Handle);
        if (State == nullptr)
        {
            return;
        }

        if (bTicking)
        {
            State->bPendingKill = true;
            return;
        }
        Registry.Destroy(Handle.Handle);
    }

    void FTweenManager::Complete(FTweenHandle Handle)
    {
        FTweenState* State = Find(Handle);
        if (State == nullptr || State->bPendingKill)
        {
            return;
        }

        if (bTicking)
        {
            State->bPendingComplete = true;
            return;
        }

        // Steps and their callbacks run here, so a kill from one of them is deferred like it is mid-tick.
        bTicking = true;
        FinishNow(*State);
        bTicking = false;

        FTweenState& Finished = Registry.Get<ECS::Write<FTweenState>>(Handle.Handle);
        TFunction<void()> Callback = Finished.bPendingKill ? TFunction<void()>() : Move(Finished.FinishedCallback);
        Registry.Destroy(Handle.Handle);
        if (Callback)
        {
            Callback();
        }
    }

    void FTweenManager::Clear()
    {
        Registry.Clear();
    }

    bool FTweenManager::IsRunning(FTweenHandle Handle) const
    {
        const FTweenState* State = Find(Handle);
        return State != nullptr && !State->bPendingKill;
    }

    void FTweenManager::PushTweener(FTweenHandle Handle, FTweener&& Tweener)
    {
        FTweenState* State = Find(Handle);
        if (State == nullptr)
        {
            return;
        }

        if (State->bParallelPending && !State->Steps.IsEmpty())
        {
            State->Steps.Last().Tweeners.Add(Move(Tweener));
        }
        else
        {
            FStep& Step = State->Steps.EmplaceGetRef();
            Step.Tweeners.Add(Move(Tweener));
        }
        State->bParallelPending = false;
    }

    FTweenManager::FTweener* FTweenManager::LastTweener(FTweenHandle Handle)
    {
        FTweenState* State = Find(Handle);
        if (State == nullptr || State->Steps.IsEmpty() || State->Steps.Last().Tweeners.IsEmpty())
        {
            return nullptr;
        }
        return &State->Steps.Last().Tweeners.Last();
    }

    float FTweenManager::StepLength(const FStep& Step)
    {
        float Length = 0.0f;
        for (const FTweener& Tweener : Step.Tweeners)
        {
            Length = Math::Max(Length, Tweener.Delay + Math::Max(Tweener.Duration, 0.0f));
        }
        return Length;
    }

    float FTweenManager::AdvanceSteps(FTweenState& State, float Remaining)
    {
        const int32 NumSteps = State.Steps.Num();

        // Looped so a frame long enough to finish a step still starts the next one in the same tick.
        while (Remaining > 0.0f && State.StepIndex < NumSteps)
        {
            const int32 Index = State.bReversed ? NumSteps - 1 - State.StepIndex : State.StepIndex;
            FStep& Step = State.Steps[Index];
            bool  bStepFinished = true;
            float StepConsumed  = 0.0f;

            for (FTweener& Tweener : Step.Tweeners)
            {
                if (Tweener.bFinished)
                {
                    continue;
                }

                const float Total = Tweener.Delay + Math::Max(Tweener.Duration, 0.0f);
                const float Before = Tweener.Elapsed;
                Tweener.Elapsed = Math::Min(Tweener.Elapsed + Remaining, Total);
                StepConsumed = Math::Max(StepConsumed, Tweener.Elapsed - Before);

                const float Active = Tweener.Elapsed - Tweener.Delay;
                if (Active < 0.0f)
                {
                    bStepFinished = false;
                    continue;
                }

                switch (Tweener.Kind)
                {
                case ETweenerKind::Value:
                {
                    float Raw = (Tweener.Duration <= TweenManagerInternal::kMinDuration)
                              ? 1.0f
                              : Math::Clamp(Active / Tweener.Duration, 0.0f, 1.0f);

                    // A backward pass mirrors time, so the value retraces its forward path.
                    if (State.bReversed)
                    {
                        Raw = 1.0f - Raw;
                    }

                    if (Tweener.Apply)
                    {
                        Tweener.Apply(Tweener.CustomEase ? Tweener.CustomEase(Raw) : Easing::Evaluate(Tweener.Transition, Tweener.EaseType, Raw));
                    }
                    break;
                }
                case ETweenerKind::Callback:
                    if (Tweener.Callback)
                    {
                        Tweener.Callback();
                    }
                    break;
                case ETweenerKind::Interval:
                    break;
                }

                if (Tweener.Elapsed >= Total)
                {
                    Tweener.bFinished = true;
                }
                else
                {
                    bStepFinished = false;
                }
            }

            if (!bStepFinished)
            {
                return 0.0f;
            }

            // Whatever the longest tweener did not use rolls into the next step.
            Remaining -= Math::Max(StepConsumed, TweenManagerInternal::kMinDuration);
            ++State.StepIndex;

            if (State.StepCallback)
            {
                State.StepCallback(Index);
            }
        }
        return Remaining;
    }

    void FTweenManager::RestartPass(FTweenState& State)
    {
        State.StepIndex = 0;
        for (FStep& Step : State.Steps)
        {
            for (FTweener& Tweener : Step.Tweeners)
            {
                Tweener.Elapsed   = 0.0f;
                Tweener.bFinished = false;
            }
        }

        if (State.LoopMode == ETweenLoopMode::Yoyo)
        {
            State.bReversed = !State.bReversed;
        }
    }

    bool FTweenManager::AdvanceTween(FTweenState& State, float DeltaTime)
    {
        if (State.bPaused)
        {
            return false;
        }

        float Remaining = DeltaTime * Math::Max(State.SpeedScale, 0.0f);
        for (int32 Pass = 0; ; ++Pass)
        {
            Remaining = AdvanceSteps(State, Remaining);
            if (State.StepIndex < State.Steps.Num())
            {
                return false;
            }

            ++State.CompletedLoops;
            if (State.LoopsRemaining != 0 && --State.LoopsRemaining == 0)
            {
                return true;
            }

            RestartPass(State);
            if (State.LoopCallback)
            {
                State.LoopCallback(State.CompletedLoops);
            }

            // What the pass did not use starts the next one, so a loop keeps its rhythm instead of drifting late.
            float PassLength = 0.0f;
            for (const FStep& Step : State.Steps)
            {
                PassLength += StepLength(Step);
            }
            if (Remaining <= 0.0f || PassLength < TweenManagerInternal::kMinCarriedPass || Pass + 1 >= TweenManagerInternal::kMaxPassesPerTick)
            {
                return false;
            }
        }
    }

    bool FTweenManager::FinishNow(FTweenState& State)
    {
        State.bPaused        = false;
        State.LoopsRemaining = 1;
        State.SpeedScale     = 1.0f;
        return AdvanceTween(State, 1e30f);
    }

    void FTweenManager::Tick(float DeltaTime, float UnscaledDeltaTime)
    {
        bTicking = true;

        auto View = Registry.View<ECS::Write<FTweenState>>();
        for (ECS::FEntity Entity : View)
        {
            FTweenState& State = View.Get<ECS::Write<FTweenState>>(Entity);

            // An owner that has gone away takes its tweens with it, so a setter cannot touch a dead entity.
            const bool bOwnerGone = State.Owner != ECS::NullEntity
                                 && WorldRegistry != nullptr
                                 && !WorldRegistry->IsValid(State.Owner);

            if (State.bPendingKill || bOwnerGone)
            {
                Completed.Add(Entity);
                continue;
            }

            const bool bDone = State.bPendingComplete
                             ? FinishNow(State)
                             : AdvanceTween(State, State.bIgnoreTimeScale ? UnscaledDeltaTime : DeltaTime);
            if (bDone)
            {
                if (State.FinishedCallback)
                {
                    State.FinishedCallback();
                }
                Completed.Add(Entity);
            }
        }

        bTicking = false;

        for (ECS::FEntity Entity : Completed)
        {
            if (Registry.IsValid(Entity))
            {
                Registry.Destroy(Entity);
            }
        }
        Completed.Reset();
    }

    //~ FTween

    FTween& FTween::AddValueTweener(float Duration, TFunction<void(float)> Apply)
    {
        if (Manager != nullptr)
        {
            FTweenManager::FTweener Tweener;
            Tweener.Kind     = FTweenManager::ETweenerKind::Value;
            Tweener.Duration = Duration;
            Tweener.Apply    = Move(Apply);
            Manager->PushTweener(Handle, Move(Tweener));
        }
        return *this;
    }

    FTween& FTween::Interval(float Duration)
    {
        if (Manager != nullptr)
        {
            FTweenManager::FTweener Tweener;
            Tweener.Kind     = FTweenManager::ETweenerKind::Interval;
            Tweener.Duration = Duration;
            Manager->PushTweener(Handle, Move(Tweener));
        }
        return *this;
    }

    FTween& FTween::Call(TFunction<void()> Callback)
    {
        if (Manager != nullptr)
        {
            FTweenManager::FTweener Tweener;
            Tweener.Kind     = FTweenManager::ETweenerKind::Callback;
            Tweener.Duration = 0.0f;
            Tweener.Callback = Move(Callback);
            Manager->PushTweener(Handle, Move(Tweener));
        }
        return *this;
    }

    FTween& FTween::AddTransformTweener(ECS::FEntity Entity, ETweenTransformChannel Channel, float Duration,
        TFunction<FVector3(const FVector3&, float)> LocationOrScale, TFunction<FQuat(const FQuat&, float)> Rotation)
    {
        if (Manager == nullptr || Manager->WorldRegistry == nullptr)
        {
            return *this;
        }

        ECS::FRegistry* Reg = Manager->WorldRegistry;

        // Start is sampled on the first apply, so a chained step begins wherever the previous one ended.
        auto StartVector   = MakeShared<TOptional<FVector3>>();
        auto StartRotation = MakeShared<TOptional<FQuat>>();
        return AddValueTweener(Duration, [Reg, Entity, Channel, LocationOrScale = Move(LocationOrScale), Rotation = Move(Rotation), StartVector, StartRotation](float Alpha)
        {
            STransformComponent* Transform = TweenManagerInternal::FindTransform(Reg, Entity);
            if (Transform == nullptr)
            {
                return;
            }

            switch (Channel)
            {
            case ETweenTransformChannel::Location:
                if (!StartVector->IsSet())
                {
                    *StartVector = Transform->GetLocalLocation();
                }
                Transform->SetLocalLocation(LocationOrScale(StartVector->GetValue(), Alpha));
                break;
            case ETweenTransformChannel::Scale:
                if (!StartVector->IsSet())
                {
                    *StartVector = Transform->GetLocalScale();
                }
                Transform->SetLocalScale(LocationOrScale(StartVector->GetValue(), Alpha));
                break;
            case ETweenTransformChannel::Rotation:
                if (!StartRotation->IsSet())
                {
                    *StartRotation = Transform->GetLocalRotation();
                }
                Transform->SetLocalRotation(Rotation(StartRotation->GetValue(), Alpha));
                break;
            }
        });
    }

    FTween& FTween::MoveTo(ECS::FEntity Entity, const FVector3& Target, float Duration)
    {
        return AddTransformTweener(Entity, ETweenTransformChannel::Location, Duration,
            [Target](const FVector3& Start, float Alpha) { return TweenDetail::Lerp(Start, Target, Alpha); }, {});
    }

    FTween& FTween::RotateTo(ECS::FEntity Entity, const FQuat& Target, float Duration)
    {
        return AddTransformTweener(Entity, ETweenTransformChannel::Rotation, Duration, {},
            [Target](const FQuat& Start, float Alpha) { return TweenDetail::Lerp(Start, Target, Alpha); });
    }

    FTween& FTween::ScaleTo(ECS::FEntity Entity, const FVector3& Target, float Duration)
    {
        return AddTransformTweener(Entity, ETweenTransformChannel::Scale, Duration,
            [Target](const FVector3& Start, float Alpha) { return TweenDetail::Lerp(Start, Target, Alpha); }, {});
    }

    FTween& FTween::MoveBy(ECS::FEntity Entity, const FVector3& Offset, float Duration)
    {
        return AddTransformTweener(Entity, ETweenTransformChannel::Location, Duration,
            [Offset](const FVector3& Start, float Alpha) { return TweenDetail::Lerp(Start, Start + Offset, Alpha); }, {});
    }

    FTween& FTween::RotateBy(ECS::FEntity Entity, const FQuat& Delta, float Duration)
    {
        return AddTransformTweener(Entity, ETweenTransformChannel::Rotation, Duration, {},
            [Delta](const FQuat& Start, float Alpha) { return TweenDetail::Lerp(Start, Start * Delta, Alpha); });
    }

    FTween& FTween::ScaleBy(ECS::FEntity Entity, const FVector3& Factor, float Duration)
    {
        return AddTransformTweener(Entity, ETweenTransformChannel::Scale, Duration,
            [Factor](const FVector3& Start, float Alpha) { return TweenDetail::Lerp(Start, Start * Factor, Alpha); }, {});
    }

    FTween& FTween::AddOffsetTweener(ECS::FEntity Entity, ETweenTransformChannel Channel, float Duration, TFunction<FVector3(float)> OffsetAt)
    {
        if (Manager == nullptr || Manager->WorldRegistry == nullptr)
        {
            return *this;
        }

        ECS::FRegistry* Reg = Manager->WorldRegistry;

        // Swaps last frame's offset for this one's, so motion from anything else still lands underneath.
        auto Last = MakeShared<FVector3>(0.0f);
        return AddValueTweener(Duration, [Reg, Entity, Channel, OffsetAt = Move(OffsetAt), Last](float Alpha)
        {
            STransformComponent* Transform = TweenManagerInternal::FindTransform(Reg, Entity);
            if (Transform == nullptr)
            {
                return;
            }

            const FVector3 Next = OffsetAt(Alpha);
            switch (Channel)
            {
            case ETweenTransformChannel::Location:
                Transform->SetLocalLocation(Transform->GetLocalLocation() - *Last + Next);
                break;
            case ETweenTransformChannel::Scale:
                Transform->SetLocalScale(Transform->GetLocalScale() - *Last + Next);
                break;
            case ETweenTransformChannel::Rotation:
                Transform->SetLocalRotation(Math::Normalize(Transform->GetLocalRotation() * Math::Inverse(TweenManagerInternal::QuatFromDegrees(*Last)) * TweenManagerInternal::QuatFromDegrees(Next)));
                break;
            }
            *Last = Next;
        });
    }

    FTween& FTween::Punch(ECS::FEntity Entity, ETweenTransformChannel Channel, const FVector3& Punch, float Duration, int32 Vibrato, float Elasticity)
    {
        const float Swings = (float)Math::Max(Vibrato, 1);
        return AddOffsetTweener(Entity, Channel, Duration, [Punch, Swings, Elasticity](float T)
        {
            // Swings back past rest by Elasticity, and the decay brings it home exactly at the end.
            float Wave = Math::Sin(Math::Pi<float>() * Swings * T);
            if (Wave < 0.0f)
            {
                Wave *= Elasticity;
            }
            return Punch * (Wave * (1.0f - T));
        });
    }

    FTween& FTween::Shake(ECS::FEntity Entity, ETweenTransformChannel Channel, const FVector3& Strength, float Duration, int32 Vibrato)
    {
        const uint32 Seed = Manager != nullptr ? Manager->NextShakeSeed++ : 1u;
        const float  Swings = (float)Math::Max(Vibrato, 1);
        const float  TwoPi = Math::TwoPi<float>();

        // Each axis runs at its own rate and phase, which reads as random without a per-frame draw.
        const FVector3 Phase(TweenManagerInternal::UnitFromSeed(Seed, 0) * TwoPi, TweenManagerInternal::UnitFromSeed(Seed, 1) * TwoPi, TweenManagerInternal::UnitFromSeed(Seed, 2) * TwoPi);
        const FVector3 Rate((0.75f + 0.5f * TweenManagerInternal::UnitFromSeed(Seed, 3)) * Swings * TwoPi,
                            (0.75f + 0.5f * TweenManagerInternal::UnitFromSeed(Seed, 4)) * Swings * TwoPi,
                            (0.75f + 0.5f * TweenManagerInternal::UnitFromSeed(Seed, 5)) * Swings * TwoPi);

        return AddOffsetTweener(Entity, Channel, Duration, [Strength, Phase, Rate, Swings](float T)
        {
            // Ramps in over the first quarter swing, so the shake does not open with a jump.
            const float Envelope = Math::Min(1.0f, T * Swings * 4.0f) * (1.0f - T);
            const FVector3 Wave(Math::Sin(Rate.x * T + Phase.x), Math::Sin(Rate.y * T + Phase.y), Math::Sin(Rate.z * T + Phase.z));
            return Strength * Wave * Envelope;
        });
    }

    template <typename T>
    FTween& FTween::AddPropertyTweener(ECS::FEntity Entity, const CStruct* Component, FStringView Path, const T& Target, float Duration)
    {
        if (Manager == nullptr || Manager->WorldRegistry == nullptr)
        {
            return *this;
        }

        TweenManagerInternal::FResolvedProperty Resolved;
        if (!TweenManagerInternal::ResolvePropertyPath(Component, Path, Resolved) || !TweenManagerInternal::TPropertyValue<T>::Accepts(Resolved.Leaf()))
        {
            LOG_WARN("Tween PropertyTo: {} has no property '{}' this value can drive", Component != nullptr ? *Component->GetName() : "a null component", FString(Path));

            // Holds the step's time, so the steps after it still start when they were built to.
            return Interval(Duration);
        }

        ECS::FRegistry* Reg = Manager->WorldRegistry;
        auto Start = MakeShared<TOptional<T>>();
        auto State = MakeShared<TweenManagerInternal::FResolvedProperty>(Move(Resolved));
        return AddValueTweener(Duration, [Reg, Entity, State, PathText = FString(Path), Target, Start](float Alpha)
        {
            TweenManagerInternal::FResolvedProperty& Live = *State;
            if (Live.Generation != FScriptStructRegistry::Get().GetGeneration())
            {
                // A reload can move or retype fields, so the path is walked again through the component's current struct.
                TweenManagerInternal::FResolvedProperty Fresh;
                const CStruct* Current = ECS::FComponentTypeRegistry::Get().GetInfo(Live.TypeID).GetStruct();
                if (!TweenManagerInternal::ResolvePropertyPath(Current, FStringView(*PathText, PathText.Len()), Fresh) || !TweenManagerInternal::TPropertyValue<T>::Accepts(Fresh.Leaf()))
                {
                    return;
                }
                Live = Move(Fresh);
            }

            const ECS::FSparseSet* Pool = Reg->IsValid(Entity) ? Reg->FindStorage(Live.TypeID) : nullptr;
            void* ComponentData = Pool != nullptr ? Pool->GetRaw(Entity) : nullptr;
            if (ComponentData == nullptr)
            {
                return;
            }

            void* Address = Live.Address(ComponentData);
            if (!Start->IsSet())
            {
                *Start = TweenManagerInternal::TPropertyValue<T>::Read(Live.Leaf(), Address);
            }
            TweenManagerInternal::TPropertyValue<T>::Write(Live.Leaf(), Address, TweenDetail::Lerp(Start->GetValue(), Target, Alpha));

            // Through the update signal, so whatever caches this component sees the change.
            Reg->PatchRaw(Live.TypeID, Entity);
        });
    }

    FTween& FTween::PropertyTo(ECS::FEntity Entity, const CStruct* Component, FStringView Path, float Target, float Duration)
    {
        return AddPropertyTweener(Entity, Component, Path, Target, Duration);
    }

    FTween& FTween::PropertyTo(ECS::FEntity Entity, const CStruct* Component, FStringView Path, const FVector2& Target, float Duration)
    {
        return AddPropertyTweener(Entity, Component, Path, Target, Duration);
    }

    FTween& FTween::PropertyTo(ECS::FEntity Entity, const CStruct* Component, FStringView Path, const FVector3& Target, float Duration)
    {
        return AddPropertyTweener(Entity, Component, Path, Target, Duration);
    }

    FTween& FTween::PropertyTo(ECS::FEntity Entity, const CStruct* Component, FStringView Path, const FVector4& Target, float Duration)
    {
        return AddPropertyTweener(Entity, Component, Path, Target, Duration);
    }

    FTween& FTween::PropertyTo(ECS::FEntity Entity, const CStruct* Component, FStringView Path, const FQuat& Target, float Duration)
    {
        return AddPropertyTweener(Entity, Component, Path, Target, Duration);
    }

    FTween& FTween::Trans(EEaseTransition Transition)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweener* Tweener = Manager->LastTweener(Handle))
            {
                Tweener->Transition = Transition;
            }
        }
        return *this;
    }

    FTween& FTween::Ease(EEaseType InEase)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweener* Tweener = Manager->LastTweener(Handle))
            {
                Tweener->EaseType = InEase;
            }
        }
        return *this;
    }

    FTween& FTween::EaseCurve(const CCurveAsset* Curve)
    {
        if (Manager == nullptr || Curve == nullptr)
        {
            return *this;
        }

        if (FTweenManager::FTweener* Tweener = Manager->LastTweener(Handle))
        {
            // Read live from the asset, so an edit to the curve shows on tweens already running.
            Tweener->CustomEase = [Asset = TStrongObjectPtr<CCurveAsset>(const_cast<CCurveAsset*>(Curve))](float Raw)
            {
                if (!Asset.IsValid())
                {
                    return Raw;
                }
                float Min = 0.0f;
                float Max = 0.0f;
                Asset->Curve.GetTimeRange(Min, Max);
                return Asset->Curve.Evaluate(Min + (Max - Min) * Raw);
            };
        }
        return *this;
    }

    FTween& FTween::EaseCurve(const SKeyedCurve& Curve)
    {
        if (Manager == nullptr)
        {
            return *this;
        }

        if (FTweenManager::FTweener* Tweener = Manager->LastTweener(Handle))
        {
            float Min = 0.0f;
            float Max = 0.0f;
            Curve.GetTimeRange(Min, Max);
            Tweener->CustomEase = [Curve, Min, Max](float Raw)
            {
                return Curve.Evaluate(Min + (Max - Min) * Raw);
            };
        }
        return *this;
    }

    FTween& FTween::Delay(float Seconds)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweener* Tweener = Manager->LastTweener(Handle))
            {
                Tweener->Delay = Math::Max(Seconds, 0.0f);
            }
        }
        return *this;
    }

    FTween& FTween::Parallel()
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->bParallelPending = true;
            }
        }
        return *this;
    }

    FTween& FTween::SetLoops(int32 Count)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->LoopsRemaining = Math::Max(Count, 0);
            }
        }
        return *this;
    }

    FTween& FTween::SetLoops(int32 Count, ETweenLoopMode Mode)
    {
        return SetLoops(Count).SetLoopMode(Mode);
    }

    FTween& FTween::SetLoopMode(ETweenLoopMode Mode)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->LoopMode = Mode;
            }
        }
        return *this;
    }

    FTween& FTween::SetSpeedScale(float Scale)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->SpeedScale = Math::Max(Scale, 0.0f);
            }
        }
        return *this;
    }

    FTween& FTween::SetIgnoreTimeScale(bool bIgnore)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->bIgnoreTimeScale = bIgnore;
            }
        }
        return *this;
    }

    FTween& FTween::OnFinished(TFunction<void()> Callback)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->FinishedCallback = Move(Callback);
            }
        }
        return *this;
    }

    FTween& FTween::OnLoop(TFunction<void(int32)> Callback)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->LoopCallback = Move(Callback);
            }
        }
        return *this;
    }

    FTween& FTween::OnStep(TFunction<void(int32)> Callback)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->StepCallback = Move(Callback);
            }
        }
        return *this;
    }

    void FTween::Kill()
    {
        if (Manager != nullptr)
        {
            Manager->Kill(Handle);
        }
    }

    void FTween::Complete()
    {
        if (Manager != nullptr)
        {
            Manager->Complete(Handle);
        }
    }

    void FTween::SetPaused(bool bPaused)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->bPaused = bPaused;
            }
        }
    }

    bool FTween::IsRunning() const
    {
        return Manager != nullptr && Manager->IsRunning(Handle);
    }

    float FTween::GetElapsed() const
    {
        const FTweenManager::FTweenState* State = Manager != nullptr ? Manager->Find(Handle) : nullptr;
        if (State == nullptr)
        {
            return 0.0f;
        }

        const int32 NumSteps = State->Steps.Num();
        float Elapsed = 0.0f;
        for (int32 Order = 0; Order < NumSteps; ++Order)
        {
            const int32 Index = State->bReversed ? NumSteps - 1 - Order : Order;
            const FTweenManager::FStep& Step = State->Steps[Index];
            if (Order < State->StepIndex)
            {
                Elapsed += FTweenManager::StepLength(Step);
                continue;
            }

            float Current = 0.0f;
            for (const FTweenManager::FTweener& Tweener : Step.Tweeners)
            {
                Current = Math::Max(Current, Tweener.Elapsed);
            }
            Elapsed += Current;
            break;
        }
        return Elapsed;
    }

    float FTween::GetDuration() const
    {
        const FTweenManager::FTweenState* State = Manager != nullptr ? Manager->Find(Handle) : nullptr;
        if (State == nullptr)
        {
            return 0.0f;
        }

        float Duration = 0.0f;
        for (const FTweenManager::FStep& Step : State->Steps)
        {
            Duration += FTweenManager::StepLength(Step);
        }
        return Duration;
    }

    float FTween::GetProgress() const
    {
        const float Duration = GetDuration();
        return Duration > 0.0f ? Math::Clamp(GetElapsed() / Duration, 0.0f, 1.0f) : 0.0f;
    }

    int32 FTween::GetCompletedLoops() const
    {
        const FTweenManager::FTweenState* State = Manager != nullptr ? Manager->Find(Handle) : nullptr;
        return State != nullptr ? State->CompletedLoops : 0;
    }
}
