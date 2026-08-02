#pragma once

#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Core/LuminaMacros.h"
#include "entt/entt.hpp"

namespace Lumina
{
    // Opaque handle from FTimerManager::SetTimer; safe across frames. The underlying entt::entity is
    // generational, so a stale handle reports invalid via IsTimerActive even after the slot is recycled.
    struct FTimerHandle
    {
        entt::entity Handle = entt::null;

        bool IsValid() const { return Handle != entt::null; }
        void Invalidate()    { Handle = entt::null; }

        bool operator==(const FTimerHandle& Other) const { return Handle == Other.Handle; }
    };

    // Per-world timer manager: one-shot + looping callbacks against the world's delta time, advancing
    // only while unpaused. Exposed to scripts as the "Timer" global (Delay/SetTimer/ClearTimer/Wait/...).
    class RUNTIME_API FTimerManager
    {
    public:

        using FTimerCallback = TFunction<void()>;

        FTimerManager() = default;
        ~FTimerManager();
        LE_NO_COPYMOVE(FTimerManager);

        FTimerHandle SetTimer(float Rate, FTimerCallback Callback, bool bLoop = false, float FirstDelay = -1.0f);
        FTimerHandle SetTimerForEntity(entt::entity Owner, float Rate, FTimerCallback Callback, bool bLoop = false, float FirstDelay = -1.0f);

        void ClearTimer(FTimerHandle& Handle);
        void ClearTimersForEntity(entt::entity Owner);
        void Clear();

        bool  IsTimerActive(FTimerHandle Handle) const;
        bool  IsTimerPaused(FTimerHandle Handle) const;
        float GetTimerRate(FTimerHandle Handle) const;
        float GetTimerRemaining(FTimerHandle Handle) const;
        float GetTimerElapsed(FTimerHandle Handle) const;

        void SetTimerPaused(FTimerHandle Handle, bool bPause);

        void Tick(float DeltaTime);

    private:

        struct FTimer
        {
            float               Rate            = 0.0f;
            float               Remaining       = 0.0f;
            bool                bLoop           = false;
            bool                bPaused         = false;
            bool                bPendingDestroy = false;
            entt::entity        Owner           = entt::null;
            FTimerCallback      NativeCallback;
        };

        entt::entity CreateTimer(float Rate, bool bLoop, float FirstDelay, entt::entity Owner, FTimerCallback NativeCallback);

        mutable entt::registry  Registry;
        bool                    bTicking = false;
    };
}
