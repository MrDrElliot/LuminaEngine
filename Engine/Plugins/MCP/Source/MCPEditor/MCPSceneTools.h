#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPSceneTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SComponentTypeInfo
    {
        GENERATED_BODY()

        /** Type name to pass back when adding this component. */
        PROPERTY()
        FString Name;

        /** Friendlier spelling of the same type, as the editor shows it. */
        PROPERTY()
        FString DisplayName;

        /** Grouping the editor files this type under. */
        PROPERTY()
        FString Category;
    };

    REFLECT()
    struct MCPEDITOR_API SListComponentTypesParams
    {
        GENERATED_BODY()

        /** Only types whose name contains this. Empty lists every one of them. */
        PROPERTY()
        FString Contains;
    };

    REFLECT()
    struct MCPEDITOR_API SListComponentTypesResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SComponentTypeInfo> Types;
    };

    REFLECT()
    struct MCPEDITOR_API SEntityInfo
    {
        GENERATED_BODY()

        /** Opaque id to pass to any tool taking an entity. Do not build one by hand. */
        PROPERTY()
        FString Id;

        PROPERTY()
        FString Name;
    };

    REFLECT()
    struct MCPEDITOR_API SListEntitiesParams
    {
        GENERATED_BODY()

        /** Only entities whose name contains this. Empty lists them all. */
        PROPERTY()
        FString Contains;

        /** How many to return at most, so a large level cannot flood the reply. */
        PROPERTY()
        int32 Limit = 100;

        /** How many matches to skip first, so a list longer than Limit can be paged. */
        PROPERTY()
        int32 Offset = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SListEntitiesResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SEntityInfo> Entities;

        /** How many matched before the limit was applied. */
        PROPERTY()
        int32 Matched = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeEntityParams
    {
        GENERATED_BODY()

        /** Id from scene.list_entities or scene.create_entity. */
        PROPERTY()
        FString Entity;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeEntityResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        /** Type names of every component on the entity. Their values are in the text reply. */
        PROPERTY()
        TVector<FString> Components;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateEntityParams
    {
        GENERATED_BODY()

        /** Name shown in the outliner. */
        PROPERTY()
        FString Name;

        /** Component type names to attach at once, from scene.list_component_types. */
        PROPERTY()
        TVector<FString> Components;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateEntityResult
    {
        GENERATED_BODY()

        /** Id to pass to any tool taking an entity. */
        PROPERTY()
        FString Entity;

        PROPERTY()
        TVector<FString> Attached;

        /** Components that were asked for but not attached, with the reason in the text reply. */
        PROPERTY()
        TVector<FString> Skipped;
    };

    REFLECT()
    struct MCPEDITOR_API SAddComponentParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Entity;

        /** Component type name, from scene.list_component_types. */
        PROPERTY()
        FString Component;
    };

    REFLECT()
    struct MCPEDITOR_API SAddComponentResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bAdded = false;

        /** True when the entity already had it, which is reported rather than treated as a failure. */
        PROPERTY()
        bool bAlreadyPresent = false;
    };

    REFLECT()
    struct MCPEDITOR_API SAddScriptParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Entity;

        /** Script class name as the inspector's script picker shows it, such as Game.InventoryUI. */
        PROPERTY()
        FString ScriptClass;
    };

    REFLECT()
    struct MCPEDITOR_API SAddScriptResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bAdded = false;
    };

    REFLECT()
    struct MCPEDITOR_API SSetPropertyParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Entity;

        /** Component type name, as entity.describe reports it. */
        PROPERTY()
        FString Component;

        /** Field path inside the component, such as Intensity or LightColor.X or Materials[2]. */
        PROPERTY()
        FString Path;

        /** The new value as JSON, so 12.5 or "Torch" or true or {"X":1,"Y":0,"Z":0}. */
        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SSetPropertyResult
    {
        GENERATED_BODY()

        /** What the field held before the change, as JSON. */
        PROPERTY()
        FString Previous;

        /** What it holds now, read back after applying. */
        PROPERTY()
        FString Current;
    };

    REFLECT()
    struct MCPEDITOR_API SDestroyEntitiesParams
    {
        GENERATED_BODY()

        /** Ids from scene.list_entities. Children go with their parents. */
        PROPERTY()
        TVector<FString> Entities;
    };

    REFLECT()
    struct MCPEDITOR_API SDestroyEntitiesResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Destroyed = 0;

        /** Ids that named nothing, reported rather than failing the whole call. */
        PROPERTY()
        TVector<FString> Skipped;
    };

    REFLECT()
    struct MCPEDITOR_API SRemoveComponentParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Entity;

        /** Component type name, as entity.describe reports it. */
        PROPERTY()
        FString Component;
    };

    REFLECT()
    struct MCPEDITOR_API SRemoveComponentResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bRemoved = false;
    };

    namespace MCP
    {
        void RegisterSceneTools(FStringView Owner);
    }
}
