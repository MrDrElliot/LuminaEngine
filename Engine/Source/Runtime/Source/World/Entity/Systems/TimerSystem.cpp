#include "RuntimePCH.h"
#include "TimerSystem.h"

namespace Lumina
{
    void CTimerSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
    }

    void CTimerSystem::OnUpdate(const SSystemContext& Context)
    {
        Timers->Tick(Context.GetDeltaTime());
        if (!Timers->HasDue())
        {
            return;
        }

        // Callbacks run script, so they wait for the flush on the main thread.
        Context.Defer([](const SExclusiveContext& X)
        {
            X.GetRegistry().Ctx().Get<FTimerManager>().FireDue();
        });
    }
}
