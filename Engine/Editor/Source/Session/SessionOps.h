#pragma once

#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "GUID/GUID.h"
#include "World/ECS/Registry.h"

namespace Lumina
{
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

    // Null with a reason when no world editor is open.
    NODISCARD EDITOR_API ECS::FRegistry* GetSceneRegistry(FString& OutError);
    NODISCARD EDITOR_API CWorld* GetSceneWorld(FString& OutError);

    // True while a play or simulate session is running, which most mutations refuse to run during.
    NODISCARD EDITOR_API bool IsSimulating();

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
    EDITOR_API bool StopPlay(FString& OutError);
    EDITOR_API bool SetPaused(bool bPaused, FString& OutError);
    NODISCARD EDITOR_API bool IsPaused(FString& OutError);

    //~ Scene mutation, each snapshotting for undo the way the editor's own tools do.

    EDITOR_API bool RunTransacted(FName Label, const TFunction<void()>& Mutate, FString& OutError);
    EDITOR_API bool RunCreationTransacted(FName Label, const TFunction<void()>& Mutate, FString& OutError);
    EDITOR_API bool RunDestroyTransacted(FName Label, const TVector<ECS::FEntity>& Doomed,
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

    NODISCARD EDITOR_API FUndoState GetUndoState();

    // Steps actually taken, which stops early when the stack runs out.
    EDITOR_API int32 Undo(int32 Steps);
    EDITOR_API int32 Redo(int32 Steps);

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
    EDITOR_API bool CloseTab(FStringView Name, bool bDiscardUnsaved, FString& OutError);
    // Opens the asset or focuses its existing tab, reporting which tab it landed in.
    EDITOR_API bool OpenAsset(const FGuid& AssetGUID, FString& OutTabId, FString& OutError);

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
