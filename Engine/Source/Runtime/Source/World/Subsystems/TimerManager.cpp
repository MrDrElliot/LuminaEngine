#include "RuntimePCH.h"
#include "TimerManager.h"

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
        Out.Handle = CreateTimer(Rate, bLoop, FirstDelay, entt::null, std::move(Callback));
        return Out;
    }

    FTimerHandle FTimerManager::SetTimerForEntity(entt::entity Owner, float Rate, FTimerCallback Callback, bool bLoop, float FirstDelay)
    {
        FTimerHandle Out;
        Out.Handle = CreateTimer(Rate, bLoop, FirstDelay, Owner, std::move(Callback));
        return Out;
    }

    void FTimerManager::ClearTimer(FTimerHandle& Handle)
    {
        if (!Handle.IsValid() || !Registry.valid(Handle.Handle))
        {
            Handle.Invalidate();
            return;
        }

        if (bTicking)
        {
            Registry.get<FTimer>(Handle.Handle).bPendingDestroy = true;
        }
        else
        {
            Registry.destroy(Handle.Handle);
        }
        Handle.Invalidate();
    }

    void FTimerManager::ClearTimersForEntity(entt::entity Owner)
    {
        if (Owner == entt::null)
        {
            return;
        }

        auto View = Registry.view<FTimer>();
        for (entt::entity Entity : View)
        {
            FTimer& Timer = View.get<FTimer>(Entity);
            if (Timer.Owner == Owner)
            {
                if (bTicking)
                {
                    Timer.bPendingDestroy = true;
                }
                else
                {
                    Registry.destroy(Entity);
                }
            }
        }
    }

    void FTimerManager::Clear()
    {
        Registry.clear<>();
    }

    bool FTimerManager::IsTimerActive(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.valid(Handle.Handle))
        {
            return false;
        }

        const FTimer& Timer = Registry.get<FTimer>(Handle.Handle);
        return !Timer.bPendingDestroy;
    }

    bool FTimerManager::IsTimerPaused(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.valid(Handle.Handle))
        {
            return false;
        }
        return Registry.get<FTimer>(Handle.Handle).bPaused;
    }

    float FTimerManager::GetTimerRate(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.valid(Handle.Handle))
        {
            return 0.0f;
        }
        return Registry.get<FTimer>(Handle.Handle).Rate;
    }

    float FTimerManager::GetTimerRemaining(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.valid(Handle.Handle))
        {
            return 0.0f;
        }
        return Registry.get<FTimer>(Handle.Handle).Remaining;
    }

    float FTimerManager::GetTimerElapsed(FTimerHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.valid(Handle.Handle))
        {
            return 0.0f;
        }
        const FTimer& Timer = Registry.get<FTimer>(Handle.Handle);
        return Timer.Rate - Timer.Remaining;
    }

    void FTimerManager::SetTimerPaused(FTimerHandle Handle, bool bPause)
    {
        if (!Handle.IsValid() || !Registry.valid(Handle.Handle))
        {
            return;
        }
        Registry.get<FTimer>(Handle.Handle).bPaused = bPause;
    }

    void FTimerManager::Tick(float DeltaTime)
    {
        LUMINA_PROFILE_SCOPE();

        if (DeltaTime <= 0.0f)
        {
            return;
        }

        TVector<entt::entity> ToTick;
        {
            auto View = Registry.view<FTimer>();
            ToTick.reserve(View.size());
            for (entt::entity Entity : View)
            {
                ToTick.push_back(Entity);
            }
        }

        bTicking = true;

        for (entt::entity Entity : ToTick)
        {
            if (!Registry.valid(Entity))
            {
                continue;
            }

            FTimer& Timer = Registry.get<FTimer>(Entity);
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

            if (bLoop && Registry.valid(Entity))
            {
                FTimer& Live = Registry.get<FTimer>(Entity);
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

        auto DestroyView = Registry.view<FTimer>();
        for (entt::entity Entity : DestroyView)
        {
            if (DestroyView.get<FTimer>(Entity).bPendingDestroy)
            {
                Registry.destroy(Entity);
            }
        }
    }

    entt::entity FTimerManager::CreateTimer(float Rate, bool bLoop, float FirstDelay, entt::entity Owner, FTimerCallback NativeCallback)
    {
        Rate = Math::Max(Rate, 0.0f);

        if (!static_cast<bool>(NativeCallback))
        {
            LOG_WARN("[TimerManager] SetTimer called with no invokable callback - ignored.");
            return entt::null;
        }

        entt::entity Entity = Registry.create();
        FTimer& Timer = Registry.emplace<FTimer>(Entity);
        Timer.Rate           = Rate;
        Timer.Remaining      = (FirstDelay >= 0.0f) ? FirstDelay : Rate;
        Timer.bLoop          = bLoop;
        Timer.Owner          = Owner;
        Timer.NativeCallback = std::move(NativeCallback);
        return Entity;
    }
}
