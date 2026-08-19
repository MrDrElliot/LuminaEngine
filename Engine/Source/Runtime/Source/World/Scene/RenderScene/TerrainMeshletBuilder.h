#pragma once

#include "TerrainRenderTypes.h"

namespace Lumina
{
    struct STerrainComponent;

    namespace TerrainMeshletBuilder
    {
        RUNTIME_API void Build(STerrainComponent& Terrain, const FVector3& WorldOrigin);

        RUNTIME_API void UpdateRegion(STerrainComponent& Terrain, const FVector3& WorldOrigin, const FIntVector2& SampleMin, const FIntVector2& SampleMax);
    }
}
