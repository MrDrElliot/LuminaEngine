#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "MCPToolTypes.h"

#include "MCPPrefabTools.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EPrefabInstanceAction : uint8
    {
        Resync,
        Detach,
    };

    REFLECT()
    struct MCPEDITOR_API SListPrefabsParams
    {
        GENERATED_BODY()

        // Only prefabs whose name or path contains this. Empty lists all.
        PROPERTY()
        FString Contains;

        PROPERTY()
        int32 Limit = 50;

        // Matches to skip first, to page past Limit.
        PROPERTY()
        int32 Offset = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SListPrefabsResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SAssetInfo> Results;

        // How many matched before Limit was applied.
        PROPERTY()
        int32 Matched = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribePrefabParams
    {
        GENERATED_BODY()

        // Prefab GUID, from prefab.list.
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
    struct MCPEDITOR_API SPrefabNodeInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        FString StableID;

        // Empty for a root.
        PROPERTY()
        FString ParentStableID;

        PROPERTY()
        TVector<FString> Components;

        // Class names of the entity scripts, then of any held while their class is not loaded.
        PROPERTY()
        TVector<FString> Scripts;
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

        // Entities with no parent inside the prefab; usually one.
        PROPERTY()
        TVector<SPrefabEntityInfo> RootEntities;

        // GUID of the prefab this one is a variant of, or empty.
        PROPERTY()
        FString ParentPrefab;

        // Every entity, in no particular order.
        PROPERTY()
        TVector<SPrefabNodeInfo> Nodes;
    };

    REFLECT()
    struct MCPEDITOR_API SSpawnPrefabParams
    {
        GENERATED_BODY()

        // Prefab GUID, from prefab.list.
        PROPERTY()
        FString Asset;

        PROPERTY()
        SVector3Param Position;

        // Euler angles in degrees.
        PROPERTY()
        SVector3Param Rotation;

        // Uniform scale; 1 keeps the prefab's own.
        PROPERTY()
        float Scale = 1.0f;

        // Entity id to parent the instance under, or empty for the world root.
        PROPERTY()
        FString Parent;

        // Name for the spawned root; empty keeps the prefab's.
        PROPERTY()
        FString Name;
    };

    REFLECT()
    struct MCPEDITOR_API SSpawnPrefabResult
    {
        GENERATED_BODY()

        // Id of the spawned root.
        PROPERTY()
        FString Entity;

        PROPERTY()
        FString Name;
    };

    REFLECT()
    struct MCPEDITOR_API SCapturePrefabParams
    {
        GENERATED_BODY()

        // GUID of the prefab to overwrite.
        PROPERTY()
        FString Asset;

        // Root of the placed instance to capture.
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

    REFLECT()
    struct MCPEDITOR_API SCreatePrefabParams
    {
        GENERATED_BODY()

        // World entities to capture; nested picks fold into their selected ancestor. Ignored with VariantOf.
        PROPERTY()
        TVector<FString> Entities;

        // Content folder for a new prefab, /Game/Content by default. Ignored with VariantOf.
        PROPERTY()
        FString Folder;

        // Asset name. For a variant, empty means the parent's name plus _Variant.
        PROPERTY()
        FString Name;

        // GUID of a prefab to create a variant of, saved beside it.
        PROPERTY()
        FString VariantOf;
    };

    REFLECT()
    struct MCPEDITOR_API SCreatePrefabResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Path;

        PROPERTY()
        FString Guid;
    };

    REFLECT()
    struct MCPEDITOR_API SPrefabInstanceParams
    {
        GENERATED_BODY()

        PROPERTY()
        EPrefabInstanceAction Action = EPrefabInstanceAction::Resync;

        // Root of a placed instance.
        PROPERTY()
        FString Entity;
    };

    REFLECT()
    struct MCPEDITOR_API SPrefabInstanceResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bDone = false;
    };

    namespace MCP
    {
        MCPEDITOR_API void RegisterPrefabTools(FStringView Owner);
    }
}
