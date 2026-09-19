#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "MCPAssetTools.h"

#include "MCPPrefabTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SListPrefabsParams
    {
        GENERATED_BODY()

        /** Only prefabs whose name or path contains this. Empty lists them all. */
        PROPERTY()
        FString Contains;

        PROPERTY()
        int32 Limit = 50;

        /** How many matches to skip first, so a list longer than Limit can be paged. */
        PROPERTY()
        int32 Offset = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SListPrefabsResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SAssetInfo> Results;

        /** Total matches before Limit, so a caller knows to page. */
        PROPERTY()
        int32 Matched = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribePrefabParams
    {
        GENERATED_BODY()

        /** GUID of the prefab, from prefab.list. */
        PROPERTY()
        FString Asset;
    };

    REFLECT()
    struct MCPEDITOR_API SPrefabEntityInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        TVector<FString> Components;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribePrefabResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        FString Path;

        PROPERTY()
        FString ClassName;

        PROPERTY()
        int32 EntityCount = 0;

        /** Entities with no parent inside the prefab; usually one. */
        PROPERTY()
        TVector<SPrefabEntityInfo> RootEntities;

        /** GUID of the prefab this one is a variant of, or empty. */
        PROPERTY()
        FString ParentPrefab;
    };

    REFLECT()
    struct MCPEDITOR_API SVector3Param
    {
        GENERATED_BODY()

        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        float Y = 0.0f;

        PROPERTY()
        float Z = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SSpawnPrefabParams
    {
        GENERATED_BODY()

        /** GUID of the prefab, from prefab.list. */
        PROPERTY()
        FString Asset;

        PROPERTY()
        SVector3Param Position;

        /** Euler angles in degrees. */
        PROPERTY()
        SVector3Param Rotation;

        /** Uniform scale; 1 keeps the prefab's own. */
        PROPERTY()
        float Scale = 1.0f;

        /** Entity id to parent the instance under, or empty for the world root. */
        PROPERTY()
        FString Parent;

        /** Name for the spawned root; empty keeps the prefab's. */
        PROPERTY()
        FString Name;
    };

    REFLECT()
    struct MCPEDITOR_API SSpawnPrefabResult
    {
        GENERATED_BODY()

        /** Id of the spawned root, for every other entity tool. */
        PROPERTY()
        FString Entity;

        PROPERTY()
        FString Name;
    };

    REFLECT()
    struct MCPEDITOR_API SCapturePrefabParams
    {
        GENERATED_BODY()

        /** GUID of the prefab to overwrite, from prefab.list. */
        PROPERTY()
        FString Asset;

        /** Root of the placed instance to capture, from scene.list_entities or prefab.spawn. */
        PROPERTY()
        FString Entity;
    };

    REFLECT()
    struct MCPEDITOR_API SCapturePrefabResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Path;

        PROPERTY()
        int32 EntityCount = 0;
    };

    namespace MCP
    {
        void RegisterPrefabTools(FStringView Owner);
    }
}
