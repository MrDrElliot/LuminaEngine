#pragma once

#include "TerrainRenderTypes.h"
#include "Containers/Array.h"

namespace Lumina
{
    struct STerrainComponent;

    // Stateless helpers building the CPU chunk + meshlet metadata for the GPU cull pass (tight world-space
    // AABBs from the heightmap range). Run on the game thread when CPUState.bChunksDirty, then snapshotted.
    namespace TerrainMeshletBuilder
    {
        // Rebuild CPUState.Chunks/Meshlets from scratch. World-space bounds (VS's OriginXZ/TileWorldSize/MaxHeight),
        // inflated half a grid step so sliver coverage stays inside the cull AABB. Pass the entity's world origin.
        RUNTIME_API void Build(STerrainComponent& Terrain, const FVector3& WorldOrigin);

        // Partial rebuild: recomputes bounds only for chunks overlapping the sample rect [SampleMin, SampleMax].
        // Falls back to full Build if the structure doesn't match the resolution. Use after a localized edit.
        RUNTIME_API void UpdateRegion(STerrainComponent& Terrain, const FVector3& WorldOrigin, const FIntVector2& SampleMin, const FIntVector2& SampleMax);
    }
}
