#include "RuntimePCH.h"
#include "TweenSystem.h"

#include "World/Subsystems/TweenSubsystem.h"
#include "World/World.h"

namespace Lumina
{
    void STweenSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);

        // While the world is paused only the tweens that ignore time scale move, which keeps pause menus animated.
        RequireUpdate(EUpdateStage::Paused);
    }

    bool STweenSystem::HasWork(const SSystemContext& Context)
    {
        return GetWorld() != nullptr && GetWorld()->GetTweens()->GetTweenManager().HasTweens();
    }

    void STweenSystem::OnUpdate(const SSystemContext& Context)
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* World = Context.GetWorld();
        const bool bPaused = Context.GetUpdateStage() == EUpdateStage::Paused;
        World->GetTweens()->GetTweenManager().Tick(bPaused ? 0.0f : (float)Context.GetDeltaTime(), (float)World->GetRealDeltaTime());
    }
}
