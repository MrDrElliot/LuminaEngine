#pragma once
#include "EntitySystem.h"
#include "World/Subsystems/TimerManager.h"
#include "TimerSystem.generated.h"

namespace Lumina
{
    // Advances the world's timers first at FrameStart, so timer callbacks fire at the first flush, before gameplay systems.
    REFLECT()
    class CTimerSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        TResMut<FTimerManager> Timers;
    };
}
