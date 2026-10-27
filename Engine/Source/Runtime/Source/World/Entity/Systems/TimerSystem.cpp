#include "RuntimePCH.h"
#include "TimerSystem.h"
#include "World/Subsystems/TimerSubsystem.h"
#include "World/World.h"

namespace Lumina
{
    void STimerSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);

        // Callbacks fire at the batch's sync point, so the tick only counts down the manager's own timers.
        Writes<SystemResource::Timers>();
    }

    void STimerSystem::OnUpdate(const SSystemContext& Context)
    {
        LUMINA_PROFILE_SCOPE();
        Context.GetWorld()->GetTimers()->GetTimerManager().Tick((float)Context.GetDeltaTime(), &Context.GetCommandBus());
    }
}
