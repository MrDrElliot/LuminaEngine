#include "RuntimePCH.h"

#include "World/Subsystems/TweenSubsystem.h"

#include "Memory/SmartPtr.h"
#include "World/World.h"
#include "World/Entity/Components/Component.h"

namespace Lumina
{
    namespace
    {
        // Shared with whatever step captured it, so the managed handle frees when the tween is destroyed.
        using FTweenCallback = TSharedPtr<FScriptCallbackOwner>;

        FTweenCallback TweenLibraryMakeCallback(SScriptCallback Callback)
        {
            return MakeShared<FScriptCallbackOwner>(Callback);
        }

        template <typename T>
        void AddValueByAddress(FTween Builder, const T& From, const T& To, float Duration, SScriptCallback OnValue)
        {
            FTweenCallback Owner = TweenLibraryMakeCallback(OnValue);
            Builder.To(From, To, Duration, [Owner](const T& Value)
            {
                Owner->Invoke((uint64)(uintptr_t)&Value);
            });
        }

        template <typename T>
        void AddProperty(FTween Builder, ECS::FEntity Target, const FString& Component, const FString& Path, const T& To, float Duration)
        {
            Builder.PropertyTo(Target, FindComponentStruct(Component), Path, To, Duration);
        }
    }

    FTweenManager* CTweenSubsystem::EnsureTweenManager() const
    {
        if (GetWorld() == nullptr)
        {
            return nullptr;
        }
        ECS::FRegistry& Registry = GetWorld()->GetEntityRegistry();
        FTweenManager& TweenManager = Registry.Ctx().GetOrEmplace<FTweenManager>();
        TweenManager.SetWorldRegistry(&Registry);
        return &TweenManager;
    }

    void CTweenSubsystem::OnTeardown()
    {
        if (GetWorld() == nullptr)
        {
            return;
        }
        if (FTweenManager* TweenManager = GetWorld()->GetEntityRegistry().Ctx().Find<FTweenManager>())
        {
            TweenManager->Clear();
        }
    }

    FTween CTweenSubsystem::Resolve(uint32 Tween)
    {
        FTweenManager* TweenManager = EnsureTweenManager();
        return FTween(TweenManager, FTweenHandle{ ECS::FEntity::FromPacked(Tween) });
    }

    uint32 CTweenSubsystem::CreateId(ECS::FEntity Owner)
    {
        FTweenManager* TweenManager = EnsureTweenManager();
        if (TweenManager == nullptr)
        {
            return (uint32)ECS::NullEntity;
        }
        const FTween Tween = Owner != ECS::NullEntity ? TweenManager->CreateForEntity(Owner) : TweenManager->Create();
        return (uint32)Tween.GetHandle().Handle;
    }

    void CTweenSubsystem::MoveTo(uint32 Tween, ECS::FEntity Target, FVector3 Destination, float Duration)
    {
        Resolve(Tween).MoveTo(Target, Destination, Duration);
    }

    void CTweenSubsystem::RotateTo(uint32 Tween, ECS::FEntity Target, FQuat Destination, float Duration)
    {
        Resolve(Tween).RotateTo(Target, Destination, Duration);
    }

    void CTweenSubsystem::ScaleTo(uint32 Tween, ECS::FEntity Target, FVector3 Destination, float Duration)
    {
        Resolve(Tween).ScaleTo(Target, Destination, Duration);
    }

    void CTweenSubsystem::ValueTo(uint32 Tween, float From, float To, float Duration, SScriptCallback OnValue)
    {
        FTweenCallback Owner = TweenLibraryMakeCallback(OnValue);
        Resolve(Tween).To(From, To, Duration, [Owner](const float& Value)
        {
            Owner->Invoke(Scripting::PackFloatPayload(Value));
        });
    }

    void CTweenSubsystem::Interval(uint32 Tween, float Duration)
    {
        Resolve(Tween).Interval(Duration);
    }

    void CTweenSubsystem::Call(uint32 Tween, SScriptCallback OnCall)
    {
        FTweenCallback Owner = TweenLibraryMakeCallback(OnCall);
        Resolve(Tween).Call([Owner] { Owner->Invoke(0); });
    }

    void CTweenSubsystem::OnFinished(uint32 Tween, SScriptCallback OnFinished)
    {
        FTweenCallback Owner = TweenLibraryMakeCallback(OnFinished);
        Resolve(Tween).OnFinished([Owner] { Owner->Invoke(0); });
    }

    void CTweenSubsystem::Trans(uint32 Tween, EEaseTransition Transition)
    {
        Resolve(Tween).Trans(Transition);
    }

    void CTweenSubsystem::Ease(uint32 Tween, EEaseType Ease)
    {
        Resolve(Tween).Ease(Ease);
    }

    void CTweenSubsystem::Delay(uint32 Tween, float Seconds)
    {
        Resolve(Tween).Delay(Seconds);
    }

    void CTweenSubsystem::Parallel(uint32 Tween)
    {
        Resolve(Tween).Parallel();
    }

    void CTweenSubsystem::SetLoops(uint32 Tween, int32 Count)
    {
        Resolve(Tween).SetLoops(Count);
    }

    void CTweenSubsystem::SetSpeedScale(uint32 Tween, float Scale)
    {
        Resolve(Tween).SetSpeedScale(Scale);
    }

    void CTweenSubsystem::SetPaused(uint32 Tween, bool bPaused)
    {
        Resolve(Tween).SetPaused(bPaused);
    }

    void CTweenSubsystem::Kill(uint32 Tween)
    {
        Resolve(Tween).Kill();
    }

    bool CTweenSubsystem::IsRunning(uint32 Tween)
    {
        return Resolve(Tween).IsRunning();
    }

    void CTweenSubsystem::MoveBy(uint32 Tween, ECS::FEntity Target, FVector3 Offset, float Duration)
    {
        Resolve(Tween).MoveBy(Target, Offset, Duration);
    }

    void CTweenSubsystem::RotateBy(uint32 Tween, ECS::FEntity Target, FQuat Delta, float Duration)
    {
        Resolve(Tween).RotateBy(Target, Delta, Duration);
    }

    void CTweenSubsystem::ScaleBy(uint32 Tween, ECS::FEntity Target, FVector3 Factor, float Duration)
    {
        Resolve(Tween).ScaleBy(Target, Factor, Duration);
    }

    void CTweenSubsystem::Punch(uint32 Tween, ECS::FEntity Target, ETweenTransformChannel Channel, FVector3 Punch, float Duration, int32 Vibrato, float Elasticity)
    {
        Resolve(Tween).Punch(Target, Channel, Punch, Duration, Vibrato, Elasticity);
    }

    void CTweenSubsystem::Shake(uint32 Tween, ECS::FEntity Target, ETweenTransformChannel Channel, FVector3 Strength, float Duration, int32 Vibrato)
    {
        Resolve(Tween).Shake(Target, Channel, Strength, Duration, Vibrato);
    }

    void CTweenSubsystem::ValueToVector2(uint32 Tween, FVector2 From, FVector2 To, float Duration, SScriptCallback OnValue)
    {
        AddValueByAddress(Resolve(Tween), From, To, Duration, OnValue);
    }

    void CTweenSubsystem::ValueToVector3(uint32 Tween, FVector3 From, FVector3 To, float Duration, SScriptCallback OnValue)
    {
        AddValueByAddress(Resolve(Tween), From, To, Duration, OnValue);
    }

    void CTweenSubsystem::ValueToVector4(uint32 Tween, FVector4 From, FVector4 To, float Duration, SScriptCallback OnValue)
    {
        AddValueByAddress(Resolve(Tween), From, To, Duration, OnValue);
    }

    void CTweenSubsystem::ValueToQuat(uint32 Tween, FQuat From, FQuat To, float Duration, SScriptCallback OnValue)
    {
        AddValueByAddress(Resolve(Tween), From, To, Duration, OnValue);
    }

    void CTweenSubsystem::PropertyToFloat(uint32 Tween, ECS::FEntity Target, const FString& Component, const FString& Path, float To, float Duration)
    {
        AddProperty(Resolve(Tween), Target, Component, Path, To, Duration);
    }

    void CTweenSubsystem::PropertyToVector2(uint32 Tween, ECS::FEntity Target, const FString& Component, const FString& Path, FVector2 To, float Duration)
    {
        AddProperty(Resolve(Tween), Target, Component, Path, To, Duration);
    }

    void CTweenSubsystem::PropertyToVector3(uint32 Tween, ECS::FEntity Target, const FString& Component, const FString& Path, FVector3 To, float Duration)
    {
        AddProperty(Resolve(Tween), Target, Component, Path, To, Duration);
    }

    void CTweenSubsystem::PropertyToVector4(uint32 Tween, ECS::FEntity Target, const FString& Component, const FString& Path, FVector4 To, float Duration)
    {
        AddProperty(Resolve(Tween), Target, Component, Path, To, Duration);
    }

    void CTweenSubsystem::PropertyToQuat(uint32 Tween, ECS::FEntity Target, const FString& Component, const FString& Path, FQuat To, float Duration)
    {
        AddProperty(Resolve(Tween), Target, Component, Path, To, Duration);
    }

    void CTweenSubsystem::OnLoop(uint32 Tween, SScriptCallback OnLoop)
    {
        FTweenCallback Owner = TweenLibraryMakeCallback(OnLoop);
        Resolve(Tween).OnLoop([Owner](int32 Loops) { Owner->Invoke((uint64)(uint32)Loops); });
    }

    void CTweenSubsystem::OnStep(uint32 Tween, SScriptCallback OnStep)
    {
        FTweenCallback Owner = TweenLibraryMakeCallback(OnStep);
        Resolve(Tween).OnStep([Owner](int32 Step) { Owner->Invoke((uint64)(uint32)Step); });
    }

    void CTweenSubsystem::EaseCurve(uint32 Tween, CCurveAsset* Curve)
    {
        Resolve(Tween).EaseCurve(Curve);
    }

    void CTweenSubsystem::SetLoopMode(uint32 Tween, ETweenLoopMode Mode)
    {
        Resolve(Tween).SetLoopMode(Mode);
    }

    void CTweenSubsystem::SetIgnoreTimeScale(uint32 Tween, bool bIgnore)
    {
        Resolve(Tween).SetIgnoreTimeScale(bIgnore);
    }

    void CTweenSubsystem::Complete(uint32 Tween)
    {
        Resolve(Tween).Complete();
    }

    float CTweenSubsystem::GetElapsed(uint32 Tween)
    {
        return Resolve(Tween).GetElapsed();
    }

    float CTweenSubsystem::GetDuration(uint32 Tween)
    {
        return Resolve(Tween).GetDuration();
    }

    float CTweenSubsystem::GetProgress(uint32 Tween)
    {
        return Resolve(Tween).GetProgress();
    }

    int32 CTweenSubsystem::GetCompletedLoops(uint32 Tween)
    {
        return Resolve(Tween).GetCompletedLoops();
    }
}
