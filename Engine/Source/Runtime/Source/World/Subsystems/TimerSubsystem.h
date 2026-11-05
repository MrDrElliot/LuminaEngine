#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Scripting/ScriptCallback.h"
#include "World/ECS/Entity.h"
#include "TaskSystem/Coroutine.h"
#include "World/Subsystems/CoroutineManager.h"
#include "World/Subsystems/TimerManager.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "TimerSubsystem.generated.h"

namespace Lumina
{
    // World-time timers and coroutine waits, by generational id so a stale one reports inactive.
    REFLECT()
    class RUNTIME_API CTimerSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        /** Runs OnFire after Rate seconds, repeating while bLoop. FirstDelay >= 0 overrides the first wait. */
        FUNCTION()
        uint32 SetTimer(float Rate, SScriptCallback OnFire, bool bLoop = false,
            float FirstDelay = -1.0f);

        /** As SetTimer, but cleared automatically when Owner is destroyed. */
        FUNCTION()
        uint32 SetTimerForEntity(ECS::FEntity Owner, float Rate, SScriptCallback OnFire,
            bool bLoop = false, float FirstDelay = -1.0f);

        FUNCTION()
        void Clear(uint32 Timer);

        FUNCTION()
        bool IsActive(uint32 Timer);

        /** Seconds until the next fire. */
        FUNCTION()
        float GetRemaining(uint32 Timer);

        /** Seconds elapsed in the current interval. */
        FUNCTION()
        float GetElapsed(uint32 Timer);

        FUNCTION()
        void SetPaused(uint32 Timer, bool bPaused);

        /** Stops every script-owned timer, so a looper does not tick on into the next generation. */
        FUNCTION()
        static void ClearAllManaged();

        // Resumes after Ticks unpaused frames.
        FUNCTION()
        uint64 WaitTicks(int32 Ticks, SScriptCallback OnDone);

        // Resumes after Seconds measured on Clock.
        FUNCTION()
        uint64 WaitSeconds(double Seconds, ECoroutineClock Clock, SScriptCallback OnDone);

        // Drops a wait without calling it.
        FUNCTION()
        void CancelWait(uint64 Wait);

        // How many waits the world is holding, for leak checks.
        FUNCTION()
        int32 GetPendingCount();

        void OnTeardown() override;

        // The world's resources, so a system holding them and this subsystem share one instance.
        FTimerManager& GetTimerManager() { return *EnsureTimerManager(); }

        FCoroutineManager& GetCoroutineManager() { return *EnsureCoroutineManager(); }

    private:

        uint32 Schedule(ECS::FEntity Owner, float Rate, bool bLoop, float FirstDelay, SScriptCallback Callback);

        uint64 ScheduleWait(CoroDetail::FLatentWaitSpec&& Spec, SScriptCallback OnDone);

        FTimerManager*     EnsureTimerManager() const;
        FCoroutineManager* EnsureCoroutineManager() const;
    };
}
