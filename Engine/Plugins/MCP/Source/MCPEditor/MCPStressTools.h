#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPStressTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SStressSpawnParams
    {
        GENERATED_BODY()

        PROPERTY()
        int32 PointLights = 0;

        // Spot lights aim straight down with a random tilt.
        PROPERTY()
        int32 SpotLights = 0;

        // Share of the spawned lights that cast shadows, from 0 to 1.
        PROPERTY()
        float ShadowFraction = 0.0f;

        // Share of the spawned lights that scatter through fog, from 0 to 1.
        PROPERTY()
        float VolumetricFraction = 0.0f;

        // Everything lands inside a disc this many meters across the center.
        PROPERTY()
        float AreaRadius = 60.0f;

        // Each light's attenuation radius in meters, varied by up to half again either way.
        PROPERTY()
        float LightRadius = 8.0f;

        PROPERTY()
        float Intensity = 20.0f;

        // Meters above the first surface under each light.
        PROPERTY()
        float Height = 3.0f;

        // Meshes dropped onto the ground for the lights to shadow.
        PROPERTY()
        int32 Casters = 0;

        // Mesh asset GUID or path for the casters. Empty uses the engine cube.
        PROPERTY()
        FString CasterMesh;

        // Base caster size in meters, varied by up to half again either way.
        PROPERTY()
        float CasterScale = 1.5f;

        // Dynamic physics boxes dropped from above the ground, so they fall, collide and pile up.
        PROPERTY()
        int32 Bodies = 0;

        // Base body size in meters, varied by up to half again either way.
        PROPERTY()
        float BodyScale = 0.5f;

        // Meshes that bob on a sine wave every frame, which moves transforms without any physics.
        PROPERTY()
        int32 Movers = 0;

        // The same seed places everything the same way, so a before and after compare the same scene.
        PROPERTY()
        int32 Seed = 1;

        // Meters around the live camera left empty, so nothing spawns inside the view.
        PROPERTY()
        float KeepClear = 10.0f;

        // Center as [x, z]. Empty centers on the point the live camera looks at.
        PROPERTY()
        FString Center;

        // Spawns into the edited world even while a game is playing.
        PROPERTY()
        bool bEditorWorld = false;
    };

    REFLECT()
    struct MCPEDITOR_API SStressSpawnResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString World;

        PROPERTY()
        int32 Lights = 0;

        PROPERTY()
        int32 ShadowCasting = 0;

        PROPERTY()
        int32 Casters = 0;

        PROPERTY()
        int32 Bodies = 0;

        PROPERTY()
        int32 Movers = 0;

        // Every stress entity now in that world, including ones from earlier calls.
        PROPERTY()
        int32 Total = 0;

        PROPERTY()
        TVector<float> Center;
    };

    REFLECT()
    struct MCPEDITOR_API SStressClearParams
    {
        GENERATED_BODY()

        PROPERTY()
        bool bEditorWorld = false;
    };

    REFLECT()
    struct MCPEDITOR_API SStressClearResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Destroyed = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SStressGridParams
    {
        GENERATED_BODY()

        // Cells per row; cell i sits at column i % Columns and row i / Columns.
        PROPERTY()
        int32 Columns = 32;

        // Meters per cell. Each mesh covers the middle 60 percent of its cell, so a cell's center is inside it.
        PROPERTY()
        float CellSize = 2.0f;

        // Corner of cell 0 as [x, z].
        PROPERTY()
        FString Origin;

        // When set, Occupied becomes the exact set of cells holding a mesh after this call.
        PROPERTY()
        bool bSetOccupancy = false;

        PROPERTY()
        TVector<int32> Occupied;

        // Occupied cells whose mesh shifts and turns inside its cell, which dirties only transforms.
        PROPERTY()
        TVector<int32> Move;

        // Occupied cells that flip between the cube and the sphere.
        PROPERTY()
        TVector<int32> Swap;

        // Occupied cells destroyed and created again in the same frame, so the new entity can reuse the freed slot.
        PROPERTY()
        TVector<int32> Respawn;

        // Off-screen meshes added in the same frame, which grows the retained buffers under the grid's own changes.
        PROPERTY()
        int32 Decoys = 0;

        PROPERTY()
        bool bClearDecoys = false;

        PROPERTY()
        int32 Seed = 1;
    };

    REFLECT()
    struct MCPEDITOR_API SStressGridResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Created = 0;

        PROPERTY()
        int32 Destroyed = 0;

        PROPERTY()
        int32 Moved = 0;

        PROPERTY()
        int32 Swapped = 0;

        PROPERTY()
        int32 Respawned = 0;

        PROPERTY()
        int32 GridMeshes = 0;

        PROPERTY()
        int32 DecoyMeshes = 0;
    };

    namespace MCP
    {
        // Bulk scene content for stress testing, which bypasses undo so thousands of entities stay cheap to add.
        void RegisterStressTools(FStringView Owner);
    }
}
