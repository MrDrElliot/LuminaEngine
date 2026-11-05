#include "RuntimePCH.h"
#include "CoroutineSystem.h"

namespace Lumina
{
    void SCoroutineSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
        RequireUpdate(EUpdateStage::Paused);
    }

    void SCoroutineSystem::OnExclusiveUpdate(const SExclusiveContext& Context)
    {
        const bool bPaused = Context.GetUpdateStage() == EUpdateStage::Paused;
        Coroutines->Tick(Context.DeltaTime, Context.RealDeltaTime, bPaused);
    }
}
