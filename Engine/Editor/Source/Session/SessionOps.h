#pragma once

#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "GUID/GUID.h"
#include "World/ECS/Registry.h"

namespace Lumina
{
    class CEdNodeGraph;
    class CObject;
    class CStruct;
    class CWorld;
    class IEditorToolContext;
}

/**
 * The editor session as something outside the editor can drive, the way AssetOps and SceneOps already
 * expose asset and scene policy.
 *
 * Every entry point answers in plain types, so a caller never names FEditorUI, FWorldEditorTool or any
 * other UI class and is not broken by the editor rearranging them. An agent endpoint was reaching through
 * GEditorEngine and casting to the concrete UI in six different files before this existed.
 */
namespace Lumina::SessionOps
{
    // What every entry point here reports when no world editor is open.
    EDITOR_API extern const char* const GNoSceneEditor;

    // What AssetOps takes to route an asset operation through the open editor. Null when there is no UI.
    NODISCARD EDITOR_API IEditorToolContext* GetToolContext();

    //~ The scene the editor is showing.

    NODISCARD EDITOR_API bool HasSceneEditor();

    // Points the scene, entity and undo calls below at another scene tab, such as a prefab editor; empty restores the world editor.
    EDITOR_API bool SetSceneTarget(FStringView TabName, FString& OutError);
    NODISCARD EDITOR_API FString GetSceneTarget();

    // Null with a reason when no world editor is open.
    NODISCARD EDITOR_API ECS::FRegistry* GetSceneRegistry(FString& OutError);
    NODISCARD EDITOR_API CWorld* GetSceneWorld(FString& OutError);

    // True while a play or simulate session is running, which most mutations refuse to run during.
    NODISCARD EDITOR_API bool IsSimulating();

    // Whether the scene the mutation calls target is the one playing; another scene tab, such as a prefab, never is.
    NODISCARD EDITOR_API bool IsSceneTargetSimulating();

    //~ Play session.

    struct FPlayState
    {
        bool    bPlaying = false;
        bool    bPaused  = false;

        // Package path of the world on show, empty when it has never been saved.
        FString World;
    };

    NODISCARD EDITOR_API bool GetPlayState(FPlayState& Out, FString& OutError);

    EDITOR_API bool StartPlay(FString& OutError);

    // Applies to the next StartPlay. NetMode is standalone, listen or dedicated.
    EDITOR_API bool SetPlayNetwork(int32 NumPlayers, FStringView NetMode, FString& OutError);
    EDITOR_API bool StopPlay(FString& OutError);
    EDITOR_API bool SetPaused(bool bPaused, FString& OutError);
    NODISCARD EDITOR_API bool IsPaused(FString& OutError);

    //~ Scene mutation, each snapshotting for undo the way the editor's own tools do.

    EDITOR_API bool RunTransacted(FName Label, const TFunction<void()>& Mutate, FString& OutError);
    EDITOR_API bool RunCreationTransacted(FName Label, const TFunction<void()>& Mutate, FString& OutError);
    EDITOR_API bool RunDestroyTransacted(FName Label, const TVector<ECS::FEntity>& Doomed,
        const TFunction<void()>& Mutate, FString& OutError);
    // Runs the target editor's own setup on an entity created inside RunCreationTransacted, as its create menu would.
    EDITOR_API void AdoptCreatedEntity(ECS::FEntity Entity);

    // Replaces the target scene's selection, as clicking the entities in its outliner would.
    EDITOR_API bool SelectEntities(const TVector<ECS::FEntity>& Entities, FString& OutError);

    // The outliner's reparent in the scene target, with that scene's rules, as one undo step. A null parent is the top level.
    EDITOR_API bool ReparentEntity(ECS::FEntity Entity, ECS::FEntity NewParent, bool bKeepWorldTransform, FString& OutError);

    // The add-component picker's apply in the scene target, for every target that lacks the component.
    EDITOR_API bool AddComponent(const TVector<ECS::FEntity>& Targets, CStruct* ComponentType, FString& OutError);

    // Makes a spawned subtree belong to the target scene as a content drop would; call inside RunCreationTransacted.
    EDITOR_API void AdoptSpawnedSubtree(ECS::FEntity Root);

    // Duplicates as the target editor's own Duplicate does; call inside RunCreationTransacted.
    NODISCARD EDITOR_API ECS::FEntity DuplicateEntity(ECS::FEntity Source);
    // One component's undo step, recorded as a prefab override on a placed instance the way the inspector's edits are.
    EDITOR_API bool RunComponentTransacted(FName Label, ECS::FEntity Entity, CStruct* ComponentType,
        const TFunction<void()>& Mutate, FString& OutError);
    EDITOR_API bool RemoveComponentTransacted(FName Label, ECS::FEntity Entity, CStruct* ComponentType,
        FString& OutError);

    // Transacts through whichever open editor owns Object; false when none does, so undo cannot see it.
    EDITOR_API bool RunObjectTransacted(CObject* Object, FName Label, const TFunction<void()>& Mutate);

    //~ Undo, reported and driven against the world editor.

    struct FUndoState
    {
        // Empty when that direction has nothing left on the stack.
        FString NextUndo;
        FString NextRedo;
    };

    // An empty Tab means the scene target, and any other names a tab whose own history is used.
    NODISCARD EDITOR_API FUndoState GetUndoState(FStringView Tab = FStringView());

    // Steps actually taken, which stops early when the stack runs out.
    EDITOR_API int32 Undo(int32 Steps, FStringView Tab = FStringView());
    EDITOR_API int32 Redo(int32 Steps, FStringView Tab = FStringView());

    // False with a reason when Tab names no open tab, so a caller can tell a typo from an empty history.
    NODISCARD EDITOR_API bool HasUndoTarget(FStringView Tab, FString& OutError);

    //~ Open tabs.

    struct FTabInfo
    {
        FString Name;
        FString Id;
        FString AssetGuid;
        bool    bUnsaved  = false;
        bool    bFocused  = false;
        bool    bClosable = false;
    };

    EDITOR_API void ForEachTab(const TFunction<void(const FTabInfo&)>& Functor);
    EDITOR_API bool FocusTab(FStringView Name, FString& OutError);

    //~ The node graph canvas a tab shows, such as a material or animation graph editor's.

    struct FGraphView
    {
        // False until the canvas has been drawn once, since the view only exists after a draw.
        bool  bDrawn = false;
        float MinX = 0.0f;
        float MinY = 0.0f;
        float MaxX = 0.0f;
        float MaxY = 0.0f;
        float Zoom = 1.0f;

        // Nodes whose position lies outside the visible rectangle.
        TVector<int64> Offscreen;
        int32          NodeCount = 0;
    };

    NODISCARD EDITOR_API bool GetGraphView(FStringView Tab, FGraphView& Out, FString& OutError);

    // Focuses the tab and frames these nodes on its next draw, or every node when the list is empty.
    EDITOR_API bool FrameGraph(FStringView Tab, const TVector<int64>& Nodes, FString& OutError);

    // Lays the canvas out by its links on the next draw, inputs left of what they feed, as one undo step.
    EDITOR_API bool ArrangeGraph(FStringView Tab, FString& OutError);

    // Hands a canvas of Asset to the editor that has Asset open, so a scripted edit there joins that editor's history.
    EDITOR_API void AdoptNodeGraph(CObject* Asset, CEdNodeGraph* Graph);
    EDITOR_API bool CloseTab(FStringView Name, bool bDiscardUnsaved, FString& OutError);
    // The world a tab renders, which for an asset editor is its own preview world.
    EDITOR_API CWorld* GetTabWorld(FStringView Name, FString& OutError);

    // Undocks a tab onto a screen rect over the next two frames, outside the main window as its own OS window.
    EDITOR_API bool FloatTab(FStringView Name, const FVector2& ScreenPosition, const FVector2& Size, FString& OutError);
    // Opens the asset or focuses its existing tab, reporting which tab it landed in.
    EDITOR_API bool OpenAsset(const FGuid& AssetGUID, FString& OutTabId, FString& OutError);

    // Shows a content folder in the content browser, with Search in its search box (empty clears it).
    EDITOR_API bool BrowseContentFolder(FStringView Folder, FStringView Search, FString& OutError);

    //~ Saving and quitting.

    // Package paths with unsaved changes, skipping deleted assets still awaiting their destroy.
    NODISCARD EDITOR_API TVector<FString> GetDirtyPackagePaths();

    // Saves every dirty package as File > Save All does; false when any refused, which OutFailedPaths names.
    EDITOR_API bool SaveAll(uint32& OutSaved, TVector<FString>& OutFailedPaths, FString& OutError);

    enum class EQuitUnsaved : uint8
    {
        Refuse,
        Save,
        Discard,
    };

    // Stops any play session and exits without the unsaved-changes prompt, once Unsaved has settled every dirty package.
    EDITOR_API bool Quit(EQuitUnsaved Unsaved, FString& OutError);
}
