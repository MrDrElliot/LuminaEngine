#include "RuntimePCH.h"
#include "CoroutineManager.h"

#include "Containers/Algorithm.h"

namespace Lumina
{
    FCoroutineManager::~FCoroutineManager()
    {
        Clear();
    }

    void FCoroutineManager::Add(FWait&& Wait)
    {
        const CoroDetail::FLatentWaitSpec& Spec = Wait.Spec;
        if (Spec.Kind == CoroDetail::FLatentWaitSpec::EKind::Time)
        {
            Wait.Deadline = Spec.bAbsolute ? Spec.Seconds : GetTime(Spec.Clock) + Spec.Seconds;
        }

        TScopeLock Guard(WaitLock);
        Waits.Add(Move(Wait));
    }

    void FCoroutineManager::Suspend(CoroDetail::FCoroutineState& State, CoroDetail::FLatentWaitSpec&& Spec, uint32 Serial)
    {
        FWait Wait;
        Wait.State  = CoroDetail::FStateRef(&State);
        Wait.Serial = Serial;
        Wait.Spec   = Move(Spec);
        Add(Move(Wait));
    }

    uint64 FCoroutineManager::AddWait(CoroDetail::FLatentWaitSpec&& Spec, FWaitCallback Callback)
    {
        FWait Wait;
        Wait.Spec     = Move(Spec);
        Wait.Callback = Move(Callback);
        {
            TScopeLock Guard(WaitLock);
            Wait.Id = ++NextId;
        }
        const uint64 Id = Wait.Id;
        Add(Move(Wait));
        return Id;
    }

    void FCoroutineManager::CancelWait(uint64 Id)
    {
        FWaitCallback Dropped;
        {
            TScopeLock Guard(WaitLock);
            auto It = Algo::FindIf(Waits, [Id](const FWait& Wait) { return Wait.Id == Id; });
            if (It == Waits.end())
            {
                return;
            }
            Dropped = Move(It->Callback);
            Waits.RemoveAt(static_cast<int32>(It - Waits.begin()));
        }
    }

    void FCoroutineManager::Track(CoroDetail::FCoroutineState& State)
    {
        TScopeLock Guard(TrackLock);
        Tracked.Add(&State);
    }

    void FCoroutineManager::Untrack(CoroDetail::FCoroutineState& State)
    {
        TScopeLock Guard(TrackLock);
        auto It = Algo::Find(Tracked, &State);
        if (It != Tracked.end())
        {
            *It = Tracked.Last();
            Tracked.Pop();
        }
    }

    bool FCoroutineManager::IsReady(FWait& Wait, bool bPaused) const
    {
        using EKind = CoroDetail::FLatentWaitSpec::EKind;
        switch (Wait.Spec.Kind)
        {
        case EKind::Ticks:
            return !bPaused && --Wait.Spec.Ticks <= 0;
        case EKind::Time:
            return GetTime(Wait.Spec.Clock) >= Wait.Deadline;
        case EKind::Until:
            return !bPaused && Wait.Spec.Predicate();
        default:
            return false;
        }
    }

    void FCoroutineManager::Tick(double WorldDeltaTime, double RealDeltaTime, bool bPaused)
    {
        LUMINA_PROFILE_SCOPE();

        if (!bPaused)
        {
            Clocks[(uint8)ECoroutineClock::World] += WorldDeltaTime;
        }
        Clocks[(uint8)ECoroutineClock::Unpaused] += WorldDeltaTime;
        Clocks[(uint8)ECoroutineClock::Real]     += RealDeltaTime;

        // Swapped out, so a coroutine resumed here that waits again lands in the next tick rather than this one.
        {
            TScopeLock Guard(WaitLock);
            ElementOps::SwapElements(Waits, TickingWaits);
        }

        TickingWaits.RemoveAll([this, bPaused](FWait& Wait)
        {
            if (!Wait.State)
            {
                if (!IsReady(Wait, bPaused))
                {
                    return false;
                }
                FWaitCallback Callback = Move(Wait.Callback);
                Callback(true);
                return true;
            }

            CoroDetail::FCoroutineState& State = *Wait.State.Get();
            if (!State.IsOwnerAlive())
            {
                State.ForceCancel();
            }

            const bool bDie = State.ShouldDieAtSuspension();
            if (!bDie && !IsReady(Wait, bPaused))
            {
                return !State.IsWaitingOn(Wait.Serial);
            }
            if (State.ClaimWait(Wait.Serial))
            {
                if (bDie)
                {
                    State.DestroyAsCanceled();
                }
                else
                {
                    State.Resume();
                }
            }
            return true;
        });

        TScopeLock Guard(WaitLock);
        for (FWait& Wait : TickingWaits)
        {
            Waits.Add(Move(Wait));
        }
        TickingWaits.Reset();
    }

    void FCoroutineManager::Clear()
    {
        TVector<CoroDetail::FStateRef> Bound;
        {
            TScopeLock Guard(TrackLock);
            for (CoroDetail::FCoroutineState* State : Tracked)
            {
                State->DetachFromWorld();
                Bound.Emplace(State);
            }
            Tracked.Reset();
        }
        for (CoroDetail::FStateRef& State : Bound)
        {
            State->ForceCancel();
        }

        TVector<FWait> Current;
        {
            TScopeLock Guard(WaitLock);
            Current = Move(Waits);
            Waits.Reset();
        }
        for (FWait& Wait : Current)
        {
            if (!Wait.State)
            {
                FWaitCallback Callback = Move(Wait.Callback);
                Callback(false);
            }
            else if (Wait.State->ClaimWait(Wait.Serial))
            {
                Wait.State->DestroyAsCanceled();
            }
        }
    }

    int32 FCoroutineManager::NumWaiting() const
    {
        TScopeLock Guard(WaitLock);
        return Waits.Num();
    }

    int32 FCoroutineManager::NumTracked() const
    {
        TScopeLock Guard(TrackLock);
        return Tracked.Num();
    }
}
