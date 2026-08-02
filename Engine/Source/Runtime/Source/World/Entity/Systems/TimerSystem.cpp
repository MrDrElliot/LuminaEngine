#include "RuntimePCH.h"
#include "TimerSystem.h"
#include "World/Subsystems/TimerManager.h"

namespace Lumina
{
    void STimerSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();
        Context.GetRegistry().ctx().get<FTimerManager>().Tick((float)Context.GetDeltaTime());
    }
}
