#pragma once

#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "MCPToolTypes.h"

#include "MCPTerrainTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SCreateTerrainParams
    {
        GENERATED_BODY()

        // Name shown in the outliner.
        PROPERTY()
        FString Name;

        // OS path of a heightmap image whose red channel spans zero to MaxHeight; row r lands at Z offset r.
        PROPERTY()
        FString HeightmapPath;

        // Samples per side, a power of two plus one.
        PROPERTY()
        int32 Resolution = 513;

        // Side length in world units, so samples sit TileWorldSize / (Resolution - 1) apart.
        PROPERTY()
        float TileWorldSize = 512.0f;

        // World height of a full-scale sample.
        PROPERTY()
        float MaxHeight = 256.0f;

        // Center of the terrain, whose Y is the height of a zero sample.
        PROPERTY()
        SVector3Param Center;

        // Adds a heightfield collider so characters and bodies stand on it.
        PROPERTY()
        bool bCollision = true;

        // OS paths of 8-bit images, one per paint layer in order, whose red channel becomes that layer's weight.
        PROPERTY()
        TVector<FString> LayerWeightmapPaths;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateTerrainResult
    {
        GENERATED_BODY()

        // Id to pass to any tool taking an entity.
        PROPERTY()
        FString Entity;

        PROPERTY()
        int32 SourceWidth = 0;

        PROPERTY()
        int32 SourceHeight = 0;
    };

    REFLECT()
    struct MCPEDITOR_API STerrainSculptParams
    {
        GENERATED_BODY()

        // Terrain entity id, or empty for the first terrain in the scene.
        PROPERTY()
        FString Entity;

        // Sculpt, Smooth, Flatten, Noise, Erode, Terrace or Paint, as the terrain mode's palette names them.
        PROPERTY()
        FString Mode = "Sculpt";

        // World X and Z of the brush center.
        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        float Z = 0.0f;

        PROPERTY()
        float Radius = 64.0f;

        // From 0 to 1, as the brush's strength slider.
        PROPERTY()
        float Strength = 0.5f;

        // How long the brush is held, applied as one dab.
        PROPERTY()
        float Seconds = 0.1f;

        // Lowers, carves or erases instead, as holding Alt does.
        PROPERTY()
        bool bInvert = false;

        // Flatten's target in world units above the terrain origin.
        PROPERTY()
        float FlattenHeight = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API STerrainSculptResult
    {
        GENERATED_BODY()

        // World height of the ground under the brush center after the dab.
        PROPERTY()
        float Height = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SImportFoliageParams
    {
        GENERATED_BODY()

        // Name shown in the outliner; an entity of this name with foliage is replaced.
        PROPERTY()
        FString Name;

        // OS path of a LFOL file holding mesh types and instances.
        PROPERTY()
        FString Path;
    };

    REFLECT()
    struct MCPEDITOR_API SImportFoliageResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Entity;

        PROPERTY()
        int32 Types = 0;

        PROPERTY()
        int32 Instances = 0;

        // Mesh paths that did not resolve to a static mesh; their instances are dropped.
        PROPERTY()
        TVector<FString> MissingMeshes;
    };

    namespace MCP
    {
        MCPEDITOR_API void RegisterTerrainTools(FStringView Owner);
    }
}
