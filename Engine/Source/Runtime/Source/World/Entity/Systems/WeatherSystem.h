#pragma once

#include "EntitySystem.h"
#include "SystemResources.h"
#include "WeatherState.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/WeatherComponent.h"
#include "WeatherSystem.generated.h"

namespace Lumina
{
    class CWeatherPreset;

    REFLECT()
    class RUNTIME_API SWeatherSystem : public CEntitySystem
    {
        GENERATED_BODY()

    public:

        void Configure() override;
        void OnUpdate(const SSystemContext& Context) override;

        // Deactivates the weather once no enabled weather component is left.
        bool HasWork(const SSystemContext& Context) override { return true; }

    private:

        ECS::TQuery<ECS::Read<SWeatherComponent>, ECS::Without<SDisabledTag>> Weathers;
        TResMut<FWeatherState> WeatherState;
        const CWeatherPreset* ActivePreset = nullptr;
        bool                  bHasActivePreset = false;
        FWeatherParams        TransitionFrom;
        float                 TransitionAlpha = 1.0f;
        double                GustTime = 0.0;
    };
}
