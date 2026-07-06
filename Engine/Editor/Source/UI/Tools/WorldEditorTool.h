#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "EditorTool.h"
#include "FSceneEditorTool.h"
#include "ImGuizmo.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Object/Class.h"
#include "NavMeshEditMode.h"
#include "WorldEditorMode.h"
#include "Memory/Memory.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/Properties/PropertyTable.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/WorldContext.h"

namespace Lumina
{
    class CStaticMesh;
    DECLARE_MULTICAST_DELEGATE(FOnGamePreview);
    
    // Base class for displaying and manipulating scenes.
    class FWorldEditorTool : public FSceneEditorTool, public IEditorModeContext
    {
        using Super = FSceneEditorTool;
        LUMINA_SINGLETON_EDITOR_TOOL(FWorldEditorTool)


    public:
        
        FWorldEditorTool(IEditorToolContext* Context, CWorld* InWorld);
        ~FWorldEditorTool() noexcept override = default;
        LE_NO_COPYMOVE(FWorldEditorTool);

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        bool ShouldGenerateThumbnailOnSave() const override { return true; }
        
        void Update(const FUpdateContext& UpdateContext) override;

        bool OnEvent(FEvent& Event) override;
        
        void OnEntityCreated(entt::registry& Registry, entt::entity Entity);

        const char* GetTitlebarIcon() const override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        void DrawHelpMenu() override;

        // The viewport overlay toolbar is shared (FSceneEditorTool); these hooks add the world's
        // play/simulate controls, editor-mode selector, view-mode extras, and config section.
        bool IsViewportPlaying() const override;
        void PersistGizmoSettings() override;
        void DrawViewportToolbarPlayControls(float ButtonSize) override;
        void DrawViewportToolbarModeSelector(float ButtonSize) override;
        void DrawViewModeExtraItems() override;

        void PushAddTagModal(entt::entity Entity);
        void PushAddComponentModal(entt::entity Entity);

        // GetComponentEditTargets / ApplyAddComponentToTargets / DrawAddableComponentList and the
        // CreateEntity* helpers now live in FSceneEditorTool.
        void PushRenameEntityModal(entt::entity Entity);
        void PushSaveAsAssetModal();
        void PushCreatePrefabFromSelectionModal();
        void PushCreatePrefabModalForEntity(entt::entity Entity);

        // Script context-menu helpers, shared between the outliner and viewport menus. The inline
        // assign/change dropdown is always offered; "Remove Script" only when one is present.
        void DrawScriptAttachMenuItems(entt::entity Entity);
        void AttachScriptToEntity(entt::entity Entity, const FString& ScriptClass);  // C# class name; attaches or swaps
        void RemoveScriptFromEntity(entt::entity Entity);

        // Refreshes the outliner when a script component is added/removed (the row's script toggle).
        void OnEntityScriptChanged(entt::registry& Registry, entt::entity Entity);

		void OnSave() override;

        // The world editor edits the live CWorld in place (not held as the FAssetEditorTool Asset);
        // dirtying targets the world's own package.
        CPackage* GetScenePackage() const override;

        NODISCARD bool IsAssetEditorTool() const override;
        FOnGamePreview& GetOnPreviewStartRequestedDelegate() { return OnGamePreviewStartRequested; }
        FOnGamePreview& GetOnPreviewStopRequestedDelegate() { return OnGamePreviewStopRequested; }

        // Multiplayer PIE: player 1 runs in the main viewport, players 2..N get Game Preview pop-ups.
        // FEditorUI reads these on the OnGamePreviewStartRequested broadcast to spawn the extra worlds + tools.
        NODISCARD int32 GetPIEPlayerCount() const { return PlaySettings.NumPlayers; }
        NODISCARD CWorld* GetPIESourceWorld() const { return ProxyWorld.Get(); }
        /** Net mode for a player index, derived from PlaySettings (player 0 is the server when a server mode is chosen). */
        NODISCARD ENetMode ResolvePlayerNetMode(int32 PlayerIndex) const;

        void NotifyPlayInEditorStart();
        void NotifyPlayInEditorStop();

        void SetWorld(CWorld* InWorld) override;
        
        void OnEntityDestroyed(entt::registry& Registry, entt::entity Entity);
        
        void DrawSimulationControls(float ButtonSize);

        // QoL viewport actions, all bound from the Update() key handler. Each begins/ends
        // a transaction internally where appropriate so they compose with undo/redo.
        void GroupSelectedEntities();
        void DropSelectionToFloor();
        void FrameAllEntities();
        void CopyTransformFromLastSelected();
        void PasteTransformToSelection();
        void SaveCameraBookmark(int32 Slot);
        void RecallCameraBookmark(int32 Slot);
        void DrawCursorWorldPositionOverlay(ImVec2 ViewportOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera);
        void DrawEntityDebugOverlay(ImVec2 ViewportOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera);
        void DrawOffscreenSelectionIndicators(ImVec2 ViewportOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera);
        // Net interest-management overlay: the spatial grid (occupied cells), per-client AOI circles, and
        // relevant entities coloured by LOD tier. Draws into the world if it has a live FNetWorldState.
        void DrawNetworkDebugOverlay();
        
        bool HasSimulatingWorld() const { return bSimulatingWorld || bGamePreviewRunning; }
        
        void StopAllSimulations();
        
        bool IsUnsavedDocument() override;

        // IEditorModeContext: lets an active mode wrap an interaction in the undo system.
        void BeginModeTransaction() override { BeginTransaction(); }
        void EndModeTransaction(const char* Name) override { EndTransaction(FName(Name)); }


    protected:

        void OnPostUndoRedo() override;

        // Block undo/redo during PIE: transactions target the editor source world, not the running game.
        bool AllowsUndoRedo() const override;

        // Selection model + outliner engine live in FSceneEditorTool; the world editor only
        // supplies this hook (the base selection methods call it on focus change).
        void OnSelectionChanged() override { bDetailsDirty = true; }

        void SetWorldPlayInEditor(bool bShouldPlay);
        void SetWorldNewSimulate(bool bShouldSimulate);

        /** Play-in-editor settings popup (player count + net mode), opened from the play controls. */
        void DrawPlaySettingsPopup();

        /** Rebind OnEntityCreated/OnEntityDestroyed observers to the observed world's registry. */
        void RebindRegistryObservers();
        void UnbindRegistryObservers();

        // Inspect-only world switching: point the outliner/details/selection at NewWorld without touching
        // the viewport or PIE lifecycle (those stay anchored on World). Pass null (or World) to follow World
        // again. Unbinds the old observers, clears selection/details/tree, rebinds, and resyncs selection.
        void SetObservedWorld(CWorld* NewWorld);

        // Outliner row that lists every live play world (server/clients) when more than one exists, so the
        // user can switch which world's scene graph is shown. Calls SetObservedWorld on pick.
        void DrawOutlinerWorldSelector() override;

        /** Engine-driven world travel: drop everything tied to OldWorld and re-bind to NewWorld. ProxyWorld is preserved. */
        void OnWorldTravelled(CWorld* OldWorld, CWorld* NewWorld);

        /** Gameplay quit (Game.Quit): raised mid-world-tick, so only flag it; Update ends PIE at FrameStart. */
        void OnGameQuitRequested() { bGameQuitRequested = true; }

        /** Accept a content-browser drag payload in the current scope and, if it's a prefab, instantiate it under DropTarget. */
        void AcceptContentBrowserPrefabPayload(entt::entity DropTarget);

        // The Scene Graph panel + shared Add menu + filter UI + the incremental outliner engine now
        // live in FSceneEditorTool. The world editor supplies the empty-area drop + prefab instantiation
        // hooks and drives the tree via its registry observers below.
        void HandleOutlinerEmptyAreaDrop() override;

        void HandleEntityEditorDragDrop(FTreeListView& Tree, entt::entity DropItem);
        void HandlePrefabContentDrop(FStringView VirtualPath, entt::entity DropTarget) override;

        void DrawWorldSettings(bool bFocused);
        void DrawSystemsPanel(bool bFocused);
        void DrawEntityActionButtons(entt::entity Entity);
        void DrawTagList(entt::entity Entity);

        // The shared details panel lives in FSceneEditorTool; the world editor adds the Tags UI
        // through these hooks (the add-tag header button + the tag-chip section).
        void DrawDetailsHeaderExtraButtons(entt::entity Entity) override;
        void DrawDetailsExtraSections(entt::entity Entity) override;

        /** Toggle the editor's "game view": hide grid, billboards, AABBs, gizmos so the
         *  viewport shows only what a runtime camera would. Bound to G by default. */
        void ToggleGameViewMode();

    private:

        /** Register the tool's keyboard shortcuts with the FEditorAction registry.
         *  Called from OnInitialize. */
        void RegisterEditorActions();

        /** Build the editor-mode registry. Selection mode is always first; new modes
         *  are added by appending to EditorModes here. */
        void RegisterEditorModes();

        /** Current viewport mode. Always non-null after OnInitialize. */
        NODISCARD IWorldEditorMode* GetActiveMode() const;

        /** Switch the active mode by registry index. Drives OnEnter/OnExit and
         *  clears any half-finished gizmo / vertex-snap state from the prior mode. */
        void SetActiveMode(int32 NewIndex);

    private:

        struct FSelectionBox
        {
            bool bActive = false;
            ImVec2 Start;
            ImVec2 Current;
        } SelectionBox;
        
        TObjectPtr<CWorld>                      ProxyWorld;

        // Hidden, non-rendered server world spawned for "Play as Dedicated Server" (owned by
        // FWorldManager via StartPIE/StopPIE). Null unless the PIE net mode is DedicatedServer.
        CWorld*                                 PIEDedicatedServerWorld = nullptr;

        // Editor entity in ProxyWorld, tracked separately from EditorEntity (active World) so
        // PIE/Simulate can restore the editor world even if Travel swaps it mid-session.
        entt::entity                            ProxyEditorEntity = entt::null;

        // Play-in-editor session config, edited via the play-controls dropdown.
        struct FPlayInEditorSettings
        {
            int32    NumPlayers          = 1;                     // 1..4; player 1 = main viewport, 2..N = preview windows
            ENetMode NetMode             = ENetMode::Standalone;
            bool     bSeparateProcesses  = false;                 // reserved; not wired yet
        };
        FPlayInEditorSettings                   PlaySettings;
        static constexpr int32                  MaxPlayers = 4;

        FOnGamePreview                          OnGamePreviewStartRequested;
        FOnGamePreview                          OnGamePreviewStopRequested;

        // Gizmo op/mode/snap state + OutlinerListView/OutlinerContext/EntityToTreeNode/PendingOutlinerAdds
        // + EntityDestroyRequests now live in FSceneEditorTool.

        // The registry our entt observers are currently connected to. RebindToWorld swaps World without
        // touching observers, so we can't rely on World to find the old registry -- track it explicitly
        // and always unbind THIS one (else entering PIE leaves the editor world observed -> teardown crash).
        FEntityRegistry*                        ObservedRegistry = nullptr;

        TUniquePtr<FPropertyTable>              WorldSettingsPropertyTable;

        // Name filter for the Systems panel.
        ImGuiTextFilter                         SystemsFilter;

        TVector<TUniquePtr<IWorldEditorMode>>   EditorModes;
        int32                                   ActiveModeIndex = 0;

        FTransform                              CopiedTransform;
        bool                                    bHasCopiedTransform = false;
        
        static constexpr int32                  NumCameraBookmarks = 9;
        FTransform                              CameraBookmarks[NumCameraBookmarks];
        bool                                    bCameraBookmarkSet[NumCameraBookmarks] = {};
        
        // Camera preview (UpdateCameraPreview / DrawCameraPreviewOverlay) lives in FSceneEditorTool;
        // the world editor only adds the game-view gate + its settings persistence.
        bool AllowCameraPreview() const override { return !bGameViewMode; }
        void PersistCameraPreviewScale() override;

        bool                                    bDrawEntityDebugInfo = false;
        bool                                    bDrawNetworkDebug = false;
        bool                                    bGamePreviewRunning = false;
        bool                                    bSimulatingWorld = false;

        // Set from the raw event handler (Esc during play) and consumed in Update,
        // since stopping tears down the PIE world and shouldn't run mid-dispatch.
        bool                                    bStopPlayRequested = false;

        // Who owns the mouse/keyboard while playing.
        enum class EInputFocus                  : uint8 { Editor, Game };
        EInputFocus                             InputFocus = EInputFocus::Editor;
        
        void ApplyInputFocus();
        void SetInputFocus(EInputFocus NewFocus);
        
        bool                                    bGameViewMode = false;
        bool                                    bSavedWorldGridEnabled = true;
        bool                                    bSavedShowComponentVisualizers = true;
        bool                                    bSavedDrawBillboards = true;
        bool                                    bSavedDrawAABB = true;

        FDelegateHandle                         WorldTravelledHandle;
        FDelegateHandle                         GameQuitHandle;
        bool                                    bGameQuitRequested = false;
        
        bool                                    bVertexSnapAnchorValid = false;
        FVector3                                VertexSnapAnchorLocal = FVector3(0.0f);
        bool                                    bVertexSnapApplied = false;
        FVector3                                VertexSnapTargetWorld = FVector3(0.0f);
        FVector3                                VertexSnapAnchorWorld = FVector3(0.0f);
        float                                   VertexSnapPixelRadius = 16.0f;
    };
    
}
