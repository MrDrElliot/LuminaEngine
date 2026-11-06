#include "RuntimePCH.h"
#include "World/Subsystems/TimerSubsystem.h"

namespace Lumina
{
    namespace
    {
        // Owns the callback's handle, so a wait dropped without firing still frees it.
        struct FOneShotCallback
        {
            SScriptCallback Callback;

            explicit FOneShotCallback(SScriptCallback In) : Callback(In) {}
            FOneShotCallback(FOneShotCallback&& Other) noexcept : Callback(Other.Callback) { Other.Callback = {}; }
            FOneShotCallback(const FOneShotCallback&) = delete;
            FOneShotCallback& operator=(const FOneShotCallback&) = delete;
            FOneShotCallback& operator=(FOneShotCallback&&) = delete;
            ~FOneShotCallback() { Scripting::ReleaseScriptCallback(Callback); }

            void Fire(bool bCompleted)
            {
                SScriptCallback Taken = Callback;
                Callback = {};
                Scripting::InvokeScriptCallback(Taken, bCompleted ? 1 : 0);
            }
        };
    }

    uint64 CTimerSubsystem::ScheduleWait(CoroDetail::FLatentWaitSpec&& Spec, SScriptCallback OnDone)
    {
        FCoroutineManager* CoroutineManager = EnsureCoroutineManager();
        if (CoroutineManager == nullptr || !OnDone.IsBound())
        {
            Scripting::ReleaseScriptCallback(OnDone);
            return 0;
        }
        return CoroutineManager->AddWait(std::move(Spec), [Owned = FOneShotCallback(OnDone)](bool bCompleted) mutable
        {
            Owned.Fire(bCompleted);
        });
    }

    uint64 CTimerSubsystem::WaitTicks(int32 Ticks, SScriptCallback OnDone)
    {
        CoroDetail::FLatentWaitSpec Spec;
        Spec.Kind  = CoroDetail::FLatentWaitSpec::EKind::Ticks;
        Spec.Ticks = Ticks;
        return ScheduleWait(std::move(Spec), OnDone);
    }

    uint64 CTimerSubsystem::WaitSeconds(double Seconds, ECoroutineClock Clock, SScriptCallback OnDone)
    {
        CoroDetail::FLatentWaitSpec Spec;
        Spec.Kind    = CoroDetail::FLatentWaitSpec::EKind::Time;
        Spec.Clock   = Clock;
        Spec.Seconds = Seconds;
        return ScheduleWait(std::move(Spec), OnDone);
    }

    void CTimerSubsystem::CancelWait(uint64 Wait)
    {
        FCoroutineManager* CoroutineManager = EnsureCoroutineManager();
        if (Wait != 0 && CoroutineManager != nullptr)
        {
            CoroutineManager->CancelWait(Wait);
        }
    }

    int32 CTimerSubsystem::GetPendingCount()
    {
        FCoroutineManager* CoroutineManager = EnsureCoroutineManager();
        return CoroutineManager != nullptr ? CoroutineManager->NumWaiting() : 0;
    }
}
