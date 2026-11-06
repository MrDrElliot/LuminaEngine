#include "RuntimePCH.h"
#include "CoroutineSystem.h"
#include "TimerSystem.h"

namespace Lumina
{
    void CCoroutineSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
        RequireUpdate(EUpdateStage::Paused);

        // Deferred work from one segment flushes in no fixed order, so the callback runners keep a declared one.
        RunsAfter<CTimerSystem>();
    }

    void CCoroutineSystem::OnUpdate(const SSystemContext& Context)
    {
        // A resumed coroutine runs script, so the tick waits for the flush on the main thread.
        Context.Defer([](const SExclusiveContext& X)
        {
            X.GetRegistry().Ctx().Get<FCoroutineManager>().Tick(X.DeltaTime, X.RealDeltaTime, X.GetUpdateStage() == EUpdateStage::Paused);
        });
    }
}
