#pragma once
#include "EntitySystem.h"
#include "World/Subsystems/CoroutineManager.h"
#include "CoroutineSystem.generated.h"

namespace Lumina
{
    // Advances the world's FCoroutineManager once a frame, paused or not, so unpaused and real time waits keep running in a pause menu.
    REFLECT()
    class CCoroutineSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        TResMut<FCoroutineManager> Coroutines;
    };
}
