#pragma once

#include "TaskSystem/Coroutine.h"
#include "Containers/Function.h"
#include "Containers/Vector.h"
#include "Core/LuminaMacros.h"
#include "Core/Threading/Sync.h"
#include "World/Entity/Systems/SystemResources.h"

namespace Lumina
{
    // Per-world scheduler for latent coroutine waits and script waits, ticked once a frame by CCoroutineSystem.
    class RUNTIME_API FCoroutineManager : public ECS::FResource
    {
    public:

        // Payload true when the wait finished, false when its world went away first.
        using FWaitCallback = TMoveOnlyFunction<void(bool)>;

        FCoroutineManager() = default;
        ~FCoroutineManager();
        LE_NO_COPYMOVE(FCoroutineManager);

        void Suspend(CoroDetail::FCoroutineState& State, CoroDetail::FLatentWaitSpec&& Spec, uint32 Serial);

        // A wait with no coroutine behind it, for a script that awaits through its own task type.
        uint64 AddWait(CoroDetail::FLatentWaitSpec&& Spec, FWaitCallback Callback);
        void   CancelWait(uint64 Id);

        void Track(CoroDetail::FCoroutineState& State);
        void Untrack(CoroDetail::FCoroutineState& State);

        void Tick(double WorldDeltaTime, double RealDeltaTime, bool bPaused);

        // Cancels every coroutine bound to the world, past their guards, since nothing will tick them again.
        void Clear();

        double GetTime(ECoroutineClock Clock) const { return Clocks[(uint8)Clock]; }
        int32  NumWaiting() const;
        int32  NumTracked() const;

    private:

        struct FWait
        {
            CoroDetail::FStateRef       State;
            uint32                      Serial = 0;
            CoroDetail::FLatentWaitSpec Spec;
            double                      Deadline = 0.0;
            uint64                      Id = 0;
            FWaitCallback               Callback;
        };

        void Add(FWait&& Wait);
        bool IsReady(FWait& Wait, bool bPaused) const;

        double Clocks[3] = {};
        uint64 NextId = 0;

        mutable FMutex                          WaitLock;
        TVector<FWait>                          Waits;

        mutable FMutex                          TrackLock;
        TVector<CoroDetail::FCoroutineState*>   Tracked;
    };
}
