#pragma once

#include "TerrainRenderTypes.h"

namespace Lumina
{
    struct STerrainComponent;

    namespace TerrainMeshletBuilder
    {
        RUNTIME_API void Build(STerrainComponent& Terrain, const FVector3& WorldOrigin);

        RUNTIME_API void UpdateRegion(STerrainComponent& Terrain, const FVector3& WorldOrigin, const FIntVector2& SampleMin, const FIntVector2& SampleMax);

        // The bounds are baked in world space, so a terrain that moved shifts them rather than rescanning its heights.
        RUNTIME_API void Translate(STerrainComponent& Terrain, const FVector3& Delta);
    }
}
