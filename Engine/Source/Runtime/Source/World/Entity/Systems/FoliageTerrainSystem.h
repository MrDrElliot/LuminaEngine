#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Scene/RenderScene/RenderDirtyTracker.h"
#include "FoliageTerrainSystem.generated.h"

namespace Lumina
{

    // Re-projects follow-terrain foliage over a terrain's edited region, in the editor and at runtime, so sculpting moves foliage live.
    REFLECT()
    class RUNTIME_API CFoliageTerrainSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Read<STerrainComponent>, ECS::Read<STransformComponent>> Terrains;
        ECS::TQuery<ECS::Write<STerrainComponent>> TerrainResets;
        ECS::TQuery<ECS::Write<SFoliageComponent>> FoliageSets;
        TRes<FRenderDirtyTracker> DirtyTracker;
    };
}
