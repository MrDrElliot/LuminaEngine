#pragma once

#include "World/Entity/Systems/EntitySystem.h"
#include "Components/PrefabSpawnerComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "PrefabSpawnerSystem.generated.h"

namespace Lumina
{

    REFLECT()
    class CPrefabSpawnerSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnStartup(const SExclusiveContext& Context) override;
        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SPrefabSpawnerComponent>, ECS::Read<STransformComponent>> Spawners;
    };
}
