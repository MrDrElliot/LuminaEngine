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

        // The scaled delta already carries the world's DeltaTimeScale, so an unscaled tween needs the raw one.
        const float Scaled = (float)Context.GetDeltaTime();
        Tweens.Tick(Scaled, Scaled);
    }
}
