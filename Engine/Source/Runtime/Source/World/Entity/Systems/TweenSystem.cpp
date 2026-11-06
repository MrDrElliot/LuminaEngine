#include "RuntimePCH.h"
#include "TweenSystem.h"
#include "CoroutineSystem.h"
#include "InputSystem.h"

namespace Lumina
{
    void CTweenSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);

        // While the world is paused only the tweens that ignore time scale move, which keeps pause menus animated.
        RequireUpdate(EUpdateStage::Paused);

        RunsAfter<CCoroutineSystem, CInputSystem>();
    }

    void CTweenSystem::OnStartup(const SExclusiveContext& Context)
    {
        Tweens->SetWorldRegistry(&Context.GetRegistry());
    }

    bool CTweenSystem::HasWork(const SSystemContext&)
    {
        return Tweens->HasTweens();
    }

    void CTweenSystem::OnUpdate(const SSystemContext& Context)
    {
        Context.Defer([](const SExclusiveContext& X)
        {
            const bool bPaused = X.GetUpdateStage() == EUpdateStage::Paused;
            X.GetRegistry().Ctx().Get<FTweenManager>().Tick(bPaused ? 0.0f : X.GetDeltaTime(), X.GetRealDeltaTime());
        });
    }
}
