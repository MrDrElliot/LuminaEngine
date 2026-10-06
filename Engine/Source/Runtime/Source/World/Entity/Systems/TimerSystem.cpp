#include "RuntimePCH.h"
#include "TimerSystem.h"
#include "World/Subsystems/TimerManager.h"

namespace Lumina
{
    void STimerSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest);

        // Callbacks fire at the batch's sync point, so the tick only counts down the manager's own timers.
        Writes<SystemResource::Timers>();
    }

    void STimerSystem::OnUpdate()
    {
        const FSystemContext& Context = GetContext();

        LUMINA_PROFILE_SCOPE();
        Context.GetRegistry().Ctx().Get<FTimerManager>().Tick((float)Context.GetDeltaTime(), &Context.GetCommandBus());
    }
}
