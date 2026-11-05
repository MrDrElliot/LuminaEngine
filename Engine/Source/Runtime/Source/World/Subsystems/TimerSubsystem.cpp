#include "RuntimePCH.h"

#include "World/Subsystems/TimerSubsystem.h"

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        // Tracked so a generation unload stops the timer itself, not just the callback it can no longer run.
        struct FManagedTimer
        {
            FScriptCallbackOwner            Callback;
            TWeakObjectPtr<CTimerSubsystem> Timers;
            FTimerHandle                    Handle;

            FManagedTimer(SScriptCallback InCallback, CTimerSubsystem* InTimers)
                : Callback(InCallback)
                , Timers(InTimers)
            {
                Live().Add(this);
            }

            ~FManagedTimer() { Live().Remove(this); }

            LE_NO_COPYMOVE(FManagedTimer);

            static THashSet<FManagedTimer*>& Live()
            {
                static THashSet<FManagedTimer*> Set;
                return Set;
            }
        };

        // Shared, since a looping timer moves its callback out and back on every fire.
        FTimerManager::FTimerCallback MakeCallback(const TSharedPtr<FManagedTimer>& Timer)
        {
            return [Timer]() { Timer->Callback.Invoke(0); };
        }

        FTimerHandle ToHandle(uint32 Timer)
        {
            return FTimerHandle{ ECS::FEntity::FromPacked(Timer) };
        }
    }

    FTimerManager* CTimerSubsystem::EnsureTimerManager() const
    {
        return GetWorld() != nullptr ? &GetWorld()->GetEntityRegistry().Ctx().GetOrEmplace<FTimerManager>() : nullptr;
    }

    FCoroutineManager* CTimerSubsystem::EnsureCoroutineManager() const
    {
        return GetWorld() != nullptr ? &GetWorld()->GetEntityRegistry().Ctx().GetOrEmplace<FCoroutineManager>() : nullptr;
    }

    void CTimerSubsystem::OnTeardown()
    {
        if (GetWorld() == nullptr)
        {
            return;
        }
        ECS::FRegistryContext& Ctx = GetWorld()->GetEntityRegistry().Ctx();
        if (FTimerManager* TimerManager = Ctx.Find<FTimerManager>())
        {
            TimerManager->Clear();
        }
        if (FCoroutineManager* CoroutineManager = Ctx.Find<FCoroutineManager>())
        {
            CoroutineManager->Clear();
        }
    }

    uint32 CTimerSubsystem::Schedule(ECS::FEntity Owner, float Rate, bool bLoop, float FirstDelay, SScriptCallback Callback)
    {
        FTimerManager* TimerManager = EnsureTimerManager();
        if (TimerManager == nullptr || !Callback.IsBound())
        {
            Scripting::ReleaseScriptCallback(Callback);
            return (uint32)ECS::NullEntity;
        }

        TSharedPtr<FManagedTimer> Timer = MakeShared<FManagedTimer>(Callback, this);

        const FTimerHandle Handle = Owner.IsNull()
            ? TimerManager->SetTimer(Rate, MakeCallback(Timer), bLoop, FirstDelay)
            : TimerManager->SetTimerForEntity(Owner, Rate, MakeCallback(Timer), bLoop, FirstDelay);

        Timer->Handle = Handle;
        return (uint32)Handle.Handle;
    }

    uint32 CTimerSubsystem::SetTimer(float Rate, SScriptCallback OnFire, bool bLoop, float FirstDelay)
    {
        return Schedule(ECS::NullEntity, Rate, bLoop, FirstDelay, OnFire);
    }

    uint32 CTimerSubsystem::SetTimerForEntity(ECS::FEntity Owner, float Rate, SScriptCallback OnFire,
        bool bLoop, float FirstDelay)
    {
        if (Owner.IsNull())
        {
            Scripting::ReleaseScriptCallback(OnFire);
            return (uint32)ECS::NullEntity;
        }
        return Schedule(Owner, Rate, bLoop, FirstDelay, OnFire);
    }

    void CTimerSubsystem::Clear(uint32 Timer)
    {
        FTimerManager* TimerManager = EnsureTimerManager();
        if (TimerManager != nullptr)
        {
            FTimerHandle Handle = ToHandle(Timer);
            TimerManager->ClearTimer(Handle);
        }
    }

    bool CTimerSubsystem::IsActive(uint32 Timer)
    {
        FTimerManager* TimerManager = EnsureTimerManager();
        return TimerManager != nullptr && TimerManager->IsTimerActive(ToHandle(Timer));
    }

    float CTimerSubsystem::GetRemaining(uint32 Timer)
    {
        FTimerManager* TimerManager = EnsureTimerManager();
        return TimerManager != nullptr ? TimerManager->GetTimerRemaining(ToHandle(Timer)) : 0.0f;
    }

    float CTimerSubsystem::GetElapsed(uint32 Timer)
    {
        FTimerManager* TimerManager = EnsureTimerManager();
        return TimerManager != nullptr ? TimerManager->GetTimerElapsed(ToHandle(Timer)) : 0.0f;
    }

    void CTimerSubsystem::SetPaused(uint32 Timer, bool bPaused)
    {
        FTimerManager* TimerManager = EnsureTimerManager();
        if (TimerManager != nullptr)
        {
            TimerManager->SetTimerPaused(ToHandle(Timer), bPaused);
        }
    }

    void CTimerSubsystem::ClearAllManaged()
    {
        // Snapshotted, since clearing a timer destroys its entry and mutates the set.
        TVector<FManagedTimer*> Snapshot;
        Snapshot.Reserve(FManagedTimer::Live().Num());
        for (FManagedTimer* Timer : FManagedTimer::Live())
        {
            Snapshot.Add(Timer);
        }

        for (FManagedTimer* Timer : Snapshot)
        {
            CTimerSubsystem* Timers = Timer->Timers.Get();
            if (FTimerManager* TimerManager = Timers != nullptr ? Timers->EnsureTimerManager() : nullptr)
            {
                TimerManager->ClearTimer(Timer->Handle);
            }
        }
    }
}
