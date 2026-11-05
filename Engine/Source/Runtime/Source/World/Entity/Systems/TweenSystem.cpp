#include "RuntimePCH.h"
#include "TweenSystem.h"

namespace Lumina
{
    void STweenSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);

        // While the world is paused only the tweens that ignore time scale move, which keeps pause menus animated.
        RequireUpdate(EUpdateStage::Paused);
    }

    void STweenSystem::OnStartup(const SExclusiveContext& Context)
    {
        Tweens->SetWorldRegistry(&Context.GetRegistry());
    }

    bool STweenSystem::HasWork(const SSystemContext&)
    {
        return Tweens->HasTweens();
    }

    void STweenSystem::OnExclusiveUpdate(const SExclusiveContext& Context)
    {
        const bool bPaused = Context.GetUpdateStage() == EUpdateStage::Paused;
        Tweens->Tick(bPaused ? 0.0f : Context.GetDeltaTime(), Context.GetRealDeltaTime());
    }
}
