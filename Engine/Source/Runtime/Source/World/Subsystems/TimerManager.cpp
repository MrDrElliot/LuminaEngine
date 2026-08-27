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

        if (bTicking)
        {
            Registry.Get<FTimer>(Handle.Handle).bPendingDestroy = true;
        }
        else
        {
            Registry.Destroy(Handle.Handle);
        }
        Handle.Invalidate();
    }

    void FTimerManager::ClearTimersForEntity(ECS::FEntity Owner)
    {
        if (Owner == ECS::NullEntity)
        {
            return;
        }

        auto View = Registry.View<FTimer>();
        for (ECS::FEntity Entity : View)
        {
            FTimer& Timer = View.Get<FTimer>(Entity);
            if (Timer.Owner == Owner)
            {
                if (bTicking)
                {
                    Timer.bPendingDestroy = true;
                }
                else
                {
                    Registry.Destroy(Entity);
                }
            }
        }
    }

    void FTimerManager::Clear()
    {
        Registry.Clear();
    }

    bool FTimerManager::IsTimerActive(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return false;
        }

        const FTimer& Timer = Registry.Get<FTimer>(Handle.Handle);
        return !Timer.bPendingDestroy;
    }

    bool FTimerManager::IsTimerPaused(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return false;
        }
        return Registry.Get<FTimer>(Handle.Handle).bPaused;
    }

    float FTimerManager::GetTimerRate(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return 0.0f;
        }
        return Registry.Get<FTimer>(Handle.Handle).Rate;
    }

    float FTimerManager::GetTimerRemaining(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return 0.0f;
        }
        return Registry.Get<FTimer>(Handle.Handle).Remaining;
    }

    float FTimerManager::GetTimerElapsed(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return 0.0f;
        }
        const FTimer& Timer = Registry.Get<FTimer>(Handle.Handle);
        return Timer.Rate - Timer.Remaining;
    }

    void FTimerManager::SetTimerPaused(FTimerHandle Handle, bool bPause)
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return;
        }
        Registry.Get<FTimer>(Handle.Handle).bPaused = bPause;
    }

    void FTimerManager::Tick(float DeltaTime)
    {
        LUMINA_PROFILE_SCOPE();

        if (DeltaTime <= 0.0f)
        {
            return;
        }

        static thread_local TVector<ECS::FEntity> ToTick;
        ToTick.clear();
        {
            auto View = Registry.View<FTimer>();
            ToTick.reserve(View.Num());
            for (ECS::FEntity Entity : View)
            {
                ToTick.push_back(Entity);
            }
        }

        bTicking = true;

        for (ECS::FEntity Entity : ToTick)
        {
            if (!Registry.IsValid(Entity))
            {
                continue;
            }

            FTimer& Timer = Registry.Get<FTimer>(Entity);
            if (Timer.bPaused || Timer.bPendingDestroy)
            {
                continue;
            }

            Timer.Remaining -= DeltaTime;
            if (Timer.Remaining > 0.0f)
            {
                continue;
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

            if (bLoop && Registry.IsValid(Entity))
            {
                FTimer& Live = Registry.Get<FTimer>(Entity);
                if (!Live.bPendingDestroy)
                {
                    Live.NativeCallback = std::move(NativeCallback);
                    Live.Remaining     += Rate;
                    if (Live.Remaining <= 0.0f)
                    {
                        Live.Remaining = Rate;
                    }
                }
            }
        }

        bTicking = false;

        auto DestroyView = Registry.View<FTimer>();
        for (ECS::FEntity Entity : DestroyView)
        {
            if (DestroyView.Get<FTimer>(Entity).bPendingDestroy)
            {
                Registry.Destroy(Entity);
            }
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
