#pragma once

#include "EntitySystem.h"
#include "SystemResources.h"
#include "SystemSingletons.h"
#include "WeatherInteractionState.h"
#include "WeatherState.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Components/WeatherInteractorComponent.h"
#include "WeatherInteractionSystem.generated.h"

namespace Lumina
{

    // Tracks every Weather Interactor's contact spheres against water and ground, and soaks the entity in the weather.
    REFLECT()
    class RUNTIME_API CWeatherInteractionSystem : public CEntitySystem
    {
        GENERATED_BODY()

    public:

        void Configure() override;
        void OnUpdate(const SSystemContext& Context) override;

        // Clears last frame's contacts once no interactor is left, so nobody keeps rippling the water.
        bool HasWork(const SSystemContext& Context) override { return true; }

    private:

        ECS::TQuery<ECS::Read<SWaterComponent>, ECS::Read<STransformComponent>, ECS::Without<SDisabledTag>> WaterSources;
        ECS::TQuery<ECS::Write<SWeatherInteractorComponent>, ECS::Read<STransformComponent>, ECS::Without<SDisabledTag>> Interactors;
        ECS::TQuery<ECS::Read<SSkeletalMeshComponent>> SkeletalMeshes;
        ECS::TQuery<ECS::Read<SStaticMeshComponent>> StaticMeshes;
        TResMut<FWeatherInteractionState> InteractionState;
        TRes<FWeatherState> WeatherState;
        TRes<FResolvedSceneView> ResolvedView;
    };
}
