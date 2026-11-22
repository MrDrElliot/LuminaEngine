#pragma once

#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "ForestComponent.generated.h"

namespace Lumina
{
    // One kind of plant the forest scatters, where it may grow and how it clumps.
    REFLECT()
    struct RUNTIME_API SForestSpecies
    {
        GENERATED_BODY()

        // Mesh, density per square meter, scale range and rendering, the same settings painted foliage takes.
        PROPERTY(Editable, Category = "Species")
        SFoliageType Type;

        PROPERTY(Editable, Category = "Placement", ClampMin = 0.0f, ClampMax = 90.0f, Units = "deg")
        float MaxSlope = 35.0f;

        PROPERTY(Editable, Category = "Placement", Units = "m")
        float MinHeight = -100000.0f;

        PROPERTY(Editable, Category = "Placement", Units = "m")
        float MaxHeight = 100000.0f;

        // Wavelength of the noise that gathers this species into stands and leaves clearings between them.
        PROPERTY(Editable, Category = "Placement", ClampMin = 1.0f, Units = "m")
        float ClusterScale = 80.0f;

        // How much of the ground the stands cover, from 0 for none to 1 for an even spread.
        PROPERTY(Editable, Category = "Placement", ClampMin = 0.0f, ClampMax = 1.0f)
        float Coverage = 0.6f;
    };

    // Painted cover for one species over one square of ground, centered on 128 where the rules alone decide.
    REFLECT()
    struct RUNTIME_API SForestPaintChunk
    {
        GENERATED_BODY()

        static constexpr int32 kCells    = 32;
        static constexpr float kCellSize = 2.0f;
        static constexpr float kSize     = kCells * kCellSize;
        static constexpr uint8 kNeutral  = 128;

        // Chunk coordinates from the forest entity, so painted ground moves with it and survives a new extent.
        PROPERTY(Hidden)
        int32 X = 0;

        PROPERTY(Hidden)
        int32 Z = 0;

        PROPERTY(Hidden)
        int32 Species = 0;

        // kCells squared, row by row. Below neutral clears the species, above it fills its stands in.
        PROPERTY(Hidden)
        TVector<uint8> Weights;
    };

    // A forest kept as rules and a seed, streamed as tiles of real plants around the camera.
    REFLECT(Component, Category = "Environment")
    struct RUNTIME_API SForestComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Forest")
        TVector<SForestSpecies> Species;

        PROPERTY(Editable, Category = "Forest")
        int32 Seed = 1;

        // Side of the square the forest covers, centered on the entity.
        PROPERTY(Editable, Category = "Forest", ClampMin = 16.0f, Units = "m")
        float Extent = 2048.0f;

        // Puts each plant on the terrain under it and applies the slope and height rules.
        PROPERTY(Editable, Category = "Forest")
        bool bFollowTerrain = true;

        // Side of the smallest tile, the unit plants stream in and out of the scene by.
        PROPERTY(Editable, Category = "Streaming", ClampMin = 8.0f, Units = "m")
        float TileSize = 64.0f;

        // Furthest a tile of plants streams in, past which the forest shows nothing.
        PROPERTY(Editable, Category = "Streaming", ClampMin = 8.0f, Units = "m")
        float ViewDistance = 2048.0f;

        // Past this, a plant whose mesh has an impostor is held as a bare GPU point that only ever draws that impostor.
        PROPERTY(Editable, Category = "Streaming", ClampMin = 0.0f, Units = "m")
        float ImpostorDistance = 320.0f;

        // Tiles scattered at once on background threads.
        PROPERTY(Editable, Category = "Streaming", ClampMin = 1, ClampMax = 64)
        int32 MaxConcurrentBuilds = 16;

        PROPERTY(Hidden, Category = "Painting")
        TVector<SForestPaintChunk> Paint;

        // Plants set down one by one in world space, which keep their spot whatever the rules say.
        PROPERTY(Hidden, Category = "Painting")
        TVector<SFoliageInstance> Placed;

        static FIntVector2 GetChunkCoord(const FVector3& Center, float X, float Z);
        SForestPaintChunk* FindPaint(int32 X, int32 Z, int32 SpeciesIndex);
        SForestPaintChunk& FindOrAddPaint(int32 X, int32 Z, int32 SpeciesIndex);
        // Drops the species with its painted cover and placed plants, and shifts the ones after it down.
        void RemoveSpecies(int32 Index);
    };
}
