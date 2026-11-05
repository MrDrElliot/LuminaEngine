#pragma once

#include "World/Entity/Systems/SystemResources.h"
#include "World/ECS/Registry.h"

#include "Containers/Function.h"
#include "Containers/Vector.h"
#include "Core/LuminaMacros.h"

namespace Lumina
{
    // Opaque handle from FTimerManager::SetTimer; safe across frames. The underlying ECS::FEntity is
    // generational, so a stale handle reports invalid via IsTimerActive even after the slot is recycled.
    struct FTimerHandle
    {
        ECS::FEntity Handle = ECS::NullEntity;

        bool IsValid() const { return Handle != ECS::NullEntity; }
        void Invalidate()    { Handle = ECS::NullEntity; }

        bool operator==(const FTimerHandle& Other) const { return Handle == Other.Handle; }
    };

    // Per-world timer manager: one-shot + looping callbacks against the world's delta time, advancing
    // only while unpaused. Exposed to scripts as the "Timer" global (Delay/SetTimer/ClearTimer/Wait/...).
    class RUNTIME_API FTimerManager : public ECS::FResource
    {
    public:

        using FTimerCallback = TFunction<void()>;

        FTimerManager() = default;
        ~FTimerManager();
        LE_NO_COPYMOVE(FTimerManager);

        FTimerHandle SetTimer(float Rate, FTimerCallback Callback, bool bLoop = false, float FirstDelay = -1.0f);
        FTimerHandle SetTimerForEntity(ECS::FEntity Owner, float Rate, FTimerCallback Callback, bool bLoop = false, float FirstDelay = -1.0f);

        void ClearTimer(FTimerHandle& Handle);
        void ClearTimersForEntity(ECS::FEntity Owner);
        void Clear();

        bool  IsTimerActive(FTimerHandle Handle) const;
        bool  IsTimerPaused(FTimerHandle Handle) const;
        float GetTimerRate(FTimerHandle Handle) const;
        float GetTimerRemaining(FTimerHandle Handle) const;
        float GetTimerElapsed(FTimerHandle Handle) const;

        void SetTimerPaused(FTimerHandle Handle, bool bPause);

        // Counts down without running a callback, so it is safe beside other systems. Due timers wait for FireDue.
        void Tick(float DeltaTime);

        // Main thread. Runs the timers the last Tick found due, then re-arms or destroys each.
        void FireDue();

        NODISCARD bool HasDue() const { return !DueTimers.IsEmpty(); }

    private:

        void FireTimer(ECS::FEntity Entity);

        struct FTimer
        {
            float               Rate            = 0.0f;
            float               Remaining       = 0.0f;
            bool                bLoop           = false;
            bool                bPaused         = false;
            bool                bPendingDestroy = false;
            ECS::FEntity        Owner           = ECS::NullEntity;
            FTimerCallback      NativeCallback;
        };

        ECS::FEntity CreateTimer(float Rate, bool bLoop, float FirstDelay, ECS::FEntity Owner, FTimerCallback NativeCallback);

        mutable ECS::FRegistry  Registry;
        TVector<ECS::FEntity>   DueTimers;
    };
}
