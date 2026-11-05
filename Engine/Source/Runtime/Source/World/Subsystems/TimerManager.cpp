#include "RuntimePCH.h"
#include "TimerManager.h"
#include "World/ECS/Registry.h"

#include <algorithm>

#include "Log/Log.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    FTimerManager::~FTimerManager()
    {
        Clear();
    }

    FTimerHandle FTimerManager::SetTimer(float Rate, FTimerCallback Callback, bool bLoop, float FirstDelay)
    {
        FTimerHandle Out;
        Out.Handle = CreateTimer(Rate, bLoop, FirstDelay, ECS::NullEntity, std::move(Callback));
        return Out;
    }

    FTimerHandle FTimerManager::SetTimerForEntity(ECS::FEntity Owner, float Rate, FTimerCallback Callback, bool bLoop, float FirstDelay)
    {
        FTimerHandle Out;
        Out.Handle = CreateTimer(Rate, bLoop, FirstDelay, Owner, std::move(Callback));
        return Out;
    }

    void FTimerManager::ClearTimer(FTimerHandle& Handle)
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            Handle.Invalidate();
            return;
        }

        Registry.Destroy(Handle.Handle);
        Handle.Invalidate();
    }

    void FTimerManager::ClearTimersForEntity(ECS::FEntity Owner)
    {
        if (Owner == ECS::NullEntity)
        {
            return;
        }

        auto View = Registry.View<ECS::Write<FTimer>>();
        for (ECS::FEntity Entity : View)
        {
            FTimer& Timer = View.Get<ECS::Write<FTimer>>(Entity);
            if (Timer.Owner == Owner)
            {
                Registry.Destroy(Entity);
            }
        }
    }

    void FTimerManager::Clear()
    {
        Registry.Clear();
        DueTimers.Reset();
    }

    bool FTimerManager::IsTimerActive(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return false;
        }

        const FTimer& Timer = Registry.Get<ECS::Read<FTimer>>(Handle.Handle);
        return !Timer.bPendingDestroy;
    }

    bool FTimerManager::IsTimerPaused(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return false;
        }
        return Registry.Get<ECS::Read<FTimer>>(Handle.Handle).bPaused;
    }

    float FTimerManager::GetTimerRate(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return 0.0f;
        }
        return Registry.Get<ECS::Read<FTimer>>(Handle.Handle).Rate;
    }

    float FTimerManager::GetTimerRemaining(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return 0.0f;
        }
        return Registry.Get<ECS::Read<FTimer>>(Handle.Handle).Remaining;
    }

    float FTimerManager::GetTimerElapsed(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return 0.0f;
        }
        const FTimer& Timer = Registry.Get<ECS::Read<FTimer>>(Handle.Handle);
        return Timer.Rate - Timer.Remaining;
    }

    void FTimerManager::SetTimerPaused(FTimerHandle Handle, bool bPause)
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return;
        }
        Registry.Get<ECS::Write<FTimer>>(Handle.Handle).bPaused = bPause;
    }

    void FTimerManager::FireTimer(ECS::FEntity Entity)
    {
        if (!Registry.IsValid(Entity))
        {
            return;
        }

        FTimer& Timer = Registry.Get<ECS::Write<FTimer>>(Entity);
        if (Timer.bPaused || Timer.bPendingDestroy || Timer.Remaining > 0.0f)
        {
            return;
        }

        // Swap-out callbacks so re-entrant SetTimer/ClearTimer from within the callback is well-defined.
        FTimerCallback NativeCallback = std::move(Timer.NativeCallback);
        const bool     bLoop          = Timer.bLoop;
        const float    Rate           = Timer.Rate;

        if (!bLoop)
        {
            Timer.bPendingDestroy = true;
        }

        if (NativeCallback)
        {
            NativeCallback();
        }

        if (!Registry.IsValid(Entity))
        {
            return;
        }

        FTimer& Live = Registry.Get<ECS::Write<FTimer>>(Entity);
        if (bLoop && !Live.bPendingDestroy)
        {
            Live.NativeCallback = std::move(NativeCallback);
            Live.Remaining     += Rate;
            if (Live.Remaining <= 0.0f)
            {
                Live.Remaining = Rate;
            }
        }
        else if (Live.bPendingDestroy)
        {
            Registry.Destroy(Entity);
        }
    }

    void FTimerManager::Tick(float DeltaTime)
    {
        LUMINA_PROFILE_SCOPE();

        if (DeltaTime <= 0.0f)
        {
            return;
        }

        auto View = Registry.View<ECS::Write<FTimer>>();
        for (ECS::FEntity Entity : View)
        {
            FTimer& Timer = View.Get<ECS::Write<FTimer>>(Entity);
            if (Timer.bPaused || Timer.bPendingDestroy)
            {
                continue;
            }

            Timer.Remaining -= DeltaTime;
            if (Timer.Remaining <= 0.0f)
            {
                DueTimers.Add(Entity);
            }
        }
    }

    void FTimerManager::FireDue()
    {
        TVector<ECS::FEntity> Due = std::move(DueTimers);
        DueTimers.Reset();
        for (ECS::FEntity Entity : Due)
        {
            FireTimer(Entity);
        }
    }

    ECS::FEntity FTimerManager::CreateTimer(float Rate, bool bLoop, float FirstDelay, ECS::FEntity Owner, FTimerCallback NativeCallback)
    {
        Rate = Math::Max(Rate, 0.0f);

        if (!static_cast<bool>(NativeCallback))
        {
            LOG_WARN("[TimerManager] SetTimer called with no invokable callback - ignored.");
            return ECS::NullEntity;
        }

        ECS::FEntity Entity = Registry.Create();
        FTimer& Timer = Registry.Emplace<FTimer>(Entity);
        Timer.Rate           = Rate;
        Timer.Remaining      = (FirstDelay >= 0.0f) ? FirstDelay : Rate;
        Timer.bLoop          = bLoop;
        Timer.Owner          = Owner;
        Timer.NativeCallback = std::move(NativeCallback);
        return Entity;
    }
}
