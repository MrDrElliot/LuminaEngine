#pragma once

#include "EntitySystem.h"
#include "WeatherInteractionState.h"
#include "Core/Object/ObjectMacros.h"
#include "WeatherInteractionSystem.generated.h"

namespace Lumina
{
    // Tracks every Weather Interactor's contact spheres against water and ground, and soaks the entity in the weather.
    REFLECT()
    class RUNTIME_API SWeatherInteractionSystem : public CEntitySystem
    {
        GENERATED_BODY()

    public:

        void Configure() override;
        void OnStartup(const SSystemContext& Context) override;
        void OnUpdate(const SSystemContext& Context) override;
        bool HasWork(const SSystemContext& Context) override;

    private:

        uint32 FrameCounter = 0;
    };
}
