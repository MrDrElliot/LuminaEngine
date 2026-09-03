#include "RuntimePCH.h"
#include "TweenSystem.h"

#include "World/Subsystems/TweenManager.h"

namespace Lumina
{
    void STweenSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = Context.GetRegistry();

        FTweenManager& Tweens = Registry.Ctx().Get<FTweenManager>();
        Tweens.SetWorldRegistry(&Registry);

        Tweens.Tick((float)Context.GetDeltaTime());
    }
}
