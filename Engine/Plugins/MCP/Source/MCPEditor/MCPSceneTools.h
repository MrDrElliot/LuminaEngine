#pragma once

#include "Agent/AgentTool.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPSceneTools.generated.h"

namespace Lumina
{
    class CWorld;

    namespace ECS
    {
        class FRegistry;
    }

    REFLECT()
    enum class ESceneCameraAction : uint8
    {
        Get,
        Set,
    };

    REFLECT()
    enum class EEntityComponentAction : uint8
    {
        Add,
        Remove,
    };

    REFLECT()
    enum class EEntityScriptAction : uint8
    {
        Add,
        Remove,
    };

    REFLECT()
    enum class EEntityTagAction : uint8
    {
        Add,
        Remove,
    };

    REFLECT()
    struct MCPEDITOR_API SComponentTypeInfo
    {
        GENERATED_BODY()

        // Type name to pass when adding this component.
        PROPERTY()
        FString Name;

        PROPERTY()
        FString DisplayName;

        PROPERTY()
        FString Category;
    };

    REFLECT()
    struct MCPEDITOR_API SListComponentTypesParams
    {
        GENERATED_BODY()

        // Only types whose name contains this. Empty lists all.
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

        // Opaque id to pass to any tool taking an entity. Do not build one by hand.
        PROPERTY()
        FString Id;

        PROPERTY()
        FString Name;

        // Id of the parent entity, empty at the world root.
        PROPERTY()
        FString Parent;

        // Name of the source prefab, empty when not part of an instance.
        PROPERTY()
        FString Prefab;

        PROPERTY()
        bool bPrefabRoot = false;
    };

    REFLECT()
    struct MCPEDITOR_API SFindEntitiesParams
    {
        GENERATED_BODY()

        // Only entities whose name contains this. Empty matches all.
        PROPERTY()
        FString Contains;

        // Only entities carrying this tag. Empty matches all.
        PROPERTY()
        FString Tag;

        PROPERTY()
        int32 Limit = 100;

        // Matches to skip first, to page past Limit.
        PROPERTY()
        int32 Offset = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SFindEntitiesResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SEntityInfo> Entities;

        // How many matched before Limit was applied.
        PROPERTY()
        int32 Matched = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SSelectEntitiesParams
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<FString> Entities;
    };

    REFLECT()
    struct MCPEDITOR_API SSelectEntitiesResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Selected = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SSystemListing
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        bool bEnabled = true;

        // EUpdateStage values the system ticks in, empty for one that only starts and tears down.
        PROPERTY()
        TVector<int32> Stages;

        // Declared order, by class name.
        PROPERTY()
        TVector<FString> RunsAfter;

        PROPERTY()
        TVector<FString> RunsBefore;
    };

    REFLECT()
    struct MCPEDITOR_API SScheduledSystem
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        int32 Stage = 0;

        PROPERTY()
        int32 Batch = 0;

        PROPERTY()
        int32 BatchSize = 1;

        PROPERTY()
        bool bExclusive = false;
    };

    REFLECT()
    struct MCPEDITOR_API SListSystemsParams
    {
        GENERATED_BODY()

        // Only systems whose name contains this. Empty lists all.
        PROPERTY()
        FString Contains;
    };

    REFLECT()
    struct MCPEDITOR_API SListSystemsResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SSystemListing> Systems;

        // Placement in the running schedule, filled only for stages that have ticked.
        PROPERTY()
        TVector<SScheduledSystem> Schedule;
    };

    REFLECT()
    struct MCPEDITOR_API STagCount
    {
        GENERATED_BODY()

        PROPERTY()
        FString Tag;

        PROPERTY()
        int32 Entities = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SListTagsParams
    {
        GENERATED_BODY()

        // Only tags containing this. Empty lists all.
        PROPERTY()
        FString Contains;
    };

    REFLECT()
    struct MCPEDITOR_API SListTagsResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<STagCount> Tags;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateEntityParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // Component type names to attach, from scene.list_component_types.
        PROPERTY()
        TVector<FString> Components;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateEntityResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Entity;

        PROPERTY()
        TVector<FString> Attached;

        // Names that matched no component type.
        PROPERTY()
        TVector<FString> Skipped;
    };

    REFLECT()
    struct MCPEDITOR_API SGroundHeightParams
    {
        GENERATED_BODY()

        // Array of [x,z] points to probe.
        PROPERTY(RawJson)
        FString Points;
    };

    REFLECT()
    struct MCPEDITOR_API SGroundHeightResult
    {
        GENERATED_BODY()

        // Height of the first surface under each point, in order, or a large negative number over nothing.
        PROPERTY()
        TVector<float> Heights;
    };

    REFLECT()
    struct MCPEDITOR_API SSceneCameraParams
    {
        GENERATED_BODY()

        PROPERTY()
        ESceneCameraAction Action = ESceneCameraAction::Get;

        // Get only. Read the editor world even while a game is playing.
        PROPERTY()
        bool bEditorWorld = false;

        // Set only. [x,y,z].
        PROPERTY()
        TVector<float> Location;

        // Set only. A point [x,y,z] to face; when empty, Rotation applies.
        PROPERTY()
        TVector<float> Target;

        // Set only. Pitch, yaw and roll in degrees, as Get reports them.
        PROPERTY()
        TVector<float> Rotation;
    };

    REFLECT()
    struct MCPEDITOR_API SSceneCameraResult
    {
        GENERATED_BODY()

        // Game or Editor.
        PROPERTY()
        FString World;

        PROPERTY()
        TVector<float> Location;

        // Pitch, yaw and roll in degrees, the order a Transform track's Rotation takes.
        PROPERTY()
        TVector<float> Rotation;

        // Get only.
        PROPERTY()
        TVector<float> Forward;

        // Get only.
        PROPERTY()
        float FOV = 0.0f;

        // Get only. A shot key for this view, ready for sequence.shot.
        PROPERTY()
        FString ShotKey;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeEntityParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Entity;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeEntityResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // Type names of every component. Their values are in the text reply.
        PROPERTY()
        TVector<FString> Components;
    };

    REFLECT()
    struct MCPEDITOR_API SEntityComponentParams
    {
        GENERATED_BODY()

        PROPERTY()
        EEntityComponentAction Action = EEntityComponentAction::Add;

        PROPERTY()
        FString Entity;

        // Component type name. Remove also takes an attached script's class name, full or after the last dot.
        PROPERTY()
        FString Component;
    };

    REFLECT()
    struct MCPEDITOR_API SEntityComponentResult
    {
        GENERATED_BODY()

        // Add only.
        PROPERTY()
        bool bAdded = false;

        // Add only. The entity already had it, which is not a failure.
        PROPERTY()
        bool bAlreadyPresent = false;

        // Remove only.
        PROPERTY()
        bool bRemoved = false;
    };

    REFLECT()
    struct MCPEDITOR_API SEntityScriptParams
    {
        GENERATED_BODY()

        PROPERTY()
        EEntityScriptAction Action = EEntityScriptAction::Add;

        PROPERTY()
        FString Entity;

        // Script class name as the inspector's script picker shows it, such as Game.InventoryUI.
        PROPERTY()
        FString ScriptClass;
    };

    REFLECT()
    struct MCPEDITOR_API SEntityScriptResult
    {
        GENERATED_BODY()

        // Add only.
        PROPERTY()
        bool bAdded = false;

        // Remove only.
        PROPERTY()
        bool bRemoved = false;
    };

    REFLECT()
    struct MCPEDITOR_API SEntityTagParams
    {
        GENERATED_BODY()

        PROPERTY()
        EEntityTagAction Action = EEntityTagAction::Add;

        PROPERTY()
        TVector<FString> Entities;

        PROPERTY()
        FString Tag;
    };

    REFLECT()
    struct MCPEDITOR_API SEntityTagResult
    {
        GENERATED_BODY()

        // Entities whose tags actually changed, so a repeat call reports zero.
        PROPERTY()
        int32 Changed = 0;

        // Ids that named nothing.
        PROPERTY()
        TVector<FString> Skipped;
    };

    REFLECT()
    struct MCPEDITOR_API SSetPropertyParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Entity;

        // Component type name or attached script class, full or after the last dot, so "Game.Rpg.Enemy" or "Enemy".
        PROPERTY()
        FString Component;

        // Field path inside the component, such as Intensity, LightColor.X or Materials[2].
        PROPERTY()
        FString Path;

        // The new value as JSON, so 12.5, "Torch", true or {"X":1,"Y":0,"Z":0}.
        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SSetPropertyResult
    {
        GENERATED_BODY()

        // The field before the change, as JSON.
        PROPERTY()
        FString Previous;

        // The field read back after the change, as JSON.
        PROPERTY()
        FString Current;
    };

    REFLECT()
    struct MCPEDITOR_API SDuplicateEntityParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Entity;
    };

    REFLECT()
    struct MCPEDITOR_API SDuplicateEntityResult
    {
        GENERATED_BODY()

        // Id of the copy.
        PROPERTY()
        FString Entity;
    };

    REFLECT()
    struct MCPEDITOR_API SSetParentParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Entity;

        // The new parent, or empty for the world root.
        PROPERTY()
        FString Parent;

        // Keep the world transform; false keeps the local one, so it moves with the new parent.
        PROPERTY()
        bool bKeepWorldTransform = true;
    };

    REFLECT()
    struct MCPEDITOR_API SSetParentResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bReparented = false;
    };

    REFLECT()
    struct MCPEDITOR_API SDestroyEntitiesParams
    {
        GENERATED_BODY()

        // Children go with their parents.
        PROPERTY()
        TVector<FString> Entities;
    };

    REFLECT()
    struct MCPEDITOR_API SDestroyEntitiesResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Destroyed = 0;

        // Ids that named nothing.
        PROPERTY()
        TVector<FString> Skipped;
    };

    namespace MCP
    {
        inline constexpr float GNoGroundHeight = -100000.0f;

        void RegisterSceneTools(FStringView Owner);

        // The scene registry a mutation may edit, or null with a reason when there is none or it is the one playing.
        ECS::FRegistry* GetEditableSceneRegistry(FString& OutError);

        // The playing or simulating world, or null when no game runs.
        CWorld* FindGameWorld();

        // scene.camera's Set, which moves the editor viewport camera; call on the game thread.
        Agent::FToolResult SceneCameraSet(const SSceneCameraParams& In, SSceneCameraResult& Out);

        // The first surface under a point, in the game world when one runs, else the editor's; GNoGroundHeight over nothing.
        float GroundHeightAt(float X, float Z);
        float GroundHeightAt(CWorld* World, float X, float Z);

        inline float GroundOrZero(float Ground)
        {
            return Ground > GNoGroundHeight ? Ground : 0.0f;
        }
    }
}
