#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "EditorTool.h"
#include "FSceneEditorTool.h"
#include "ImGuizmo.h"
#include "NavMeshEditMode.h"
#include "WorldEditorMode.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Object/Class.h"
#include "Events/MouseCodes.h"
#include "Input/InputMode.h"
#include "Memory/Memory.h"
#include "Physics/Ray/RayCast.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/Properties/PropertyTable.h"
#include "UI/Widgets/CategorizedPicker.h"
#include "World/World.h"
#include "World/WorldContext.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Systems/EntitySystem.h"

namespace Lumina
{
    class CPrefab;
    class CStaticMesh;

    DECLARE_MULTICAST_DELEGATE(FOnGamePreview);

    class FWorldEditorTool : public FSceneEditorTool, public IEditorModeContext
    {
        using Super = FSceneEditorTool;

    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FWorldEditorTool)

        FWorldEditorTool(IEditorToolContext* Context, CWorld* InWorld);
        ~FWorldEditorTool() noexcept override = default;
        LE_NO_COPYMOVE(FWorldEditorTool);

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;
        bool OnEvent(FEvent& Event) override;
        void OnSave() override;
        void SetWorld(CWorld* InWorld) override;
        bool IsUnsavedDocument() override;
        NODISCARD bool IsAssetEditorTool() const override;
        bool ShouldGenerateThumbnailOnSave() const override { return true; }
        FName GetToolName() const override;
        const char* GetTitlebarIcon() const override;
        void GetDefaultCameraPose(FVector3& OutLocation, FVector3& OutTarget) const override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        void DrawViewModeExtraItems() override;
        bool IsViewportPlaying() const override;
        void PersistGizmoSettings() override;
        void DrawViewportToolbarPlayControls(float ButtonSize) override;
        void DrawViewportToolbarModeSelector(float ButtonSize) override;

        // The ejected flycam is the editor's camera, so it gets the grid, view gizmo and F-to-focus back.
        NODISCARD bool HasEditorCameraControl() const override { return FEditorTool::HasEditorCameraControl() || (bEjectedFromPlay && HasWorld()); }

        void BeginModeTransaction() override { BeginTransaction(); }
        void EndModeTransaction(const char* Name) override { EndTransaction(FName(Name)); }
        void BeginModeCreationTransaction() override { BeginCreationTransaction(); }
        ECS::FEntity GetModeSelectionFocus() const override { return GetLastSelectedEntity(); }
        void SetModeSelection(ECS::FEntity Entity) override { SetSingleSelectedEntity(Entity); }

        void OnEntityCreated(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnEntityDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnEntityScriptChanged(ECS::FRegistry& Registry, ECS::FEntity Entity);

        // Hides the grid, billboards, AABBs and gizmos so the viewport shows what a runtime camera would.
        void ToggleGameViewMode();

        // Swaps in a transient world holding the shadow range demo, discarding whatever world was open.
        void OpenShadowRangeDemo();

        void PushSaveAsAssetModal();
        void PushCreatePrefabFromSelectionModal();
        void PushCreatePrefabModalForEntity(ECS::FEntity Entity);

        // The modal's Create button, also reached from agent tools so both take one path.
        EDITOR_API CPrefab* CreatePrefabFromSelection(FStringView Folder, FStringView Name, FFixedString& OutError);
        CPrefab* CreatePrefabFromRoots(const TVector<ECS::FEntity>& Roots, const FVector3& Pivot, FStringView Folder, FStringView Name, FFixedString& OutError);
        TVector<ECS::FEntity> GatherPrefabSelectionRoots() const;
        EDITOR_API void ResyncPrefabInstance(ECS::FEntity InstanceRoot);
        EDITOR_API void DetachPrefabInstance(ECS::FEntity InstanceRoot);

        // The assign or change dropdown is always offered, Remove Script only when a script is present.
        void DrawScriptAttachMenuItems(ECS::FEntity Entity);
        void DrawAddComponentMenu(ECS::FEntity Entity);
        void AttachScriptToEntity(ECS::FEntity Entity, const FString& ScriptClass);
        void RemoveScriptFromEntity(ECS::FEntity Entity);

        FOnGamePreview& GetOnPreviewStartRequestedDelegate() { return OnGamePreviewStartRequested; }
        FOnGamePreview& GetOnPreviewStopRequestedDelegate() { return OnGamePreviewStopRequested; }

        NODISCARD int32 GetPIEPlayerCount() const { return PlaySettings.NumPlayers; }
        NODISCARD CWorld* GetPIESourceWorld() const { return ProxyWorld.Get(); }

        // Player zero is the server when a server mode is chosen.
        NODISCARD ENetMode ResolvePlayerNetMode(int32 PlayerIndex) const;

        void NotifyPlayInEditorStart();
        void NotifyPlayInEditorStop();
        bool HasSimulatingWorld() const { return bSimulatingWorld || bGamePreviewRunning; }
        EDITOR_API void StopAllSimulations();

        // Play in editor for a caller outside the tool; false when a session is already running.
        EDITOR_API bool StartPlayInEditor();

        // Players clamps to 1..MaxPlayers, and every player past the first is a client of player one.
        EDITOR_API void SetPlayNetwork(int32 NumPlayers, ENetMode NetMode);

        EDITOR_API bool IsPlaySessionPaused() const;
        EDITOR_API void SetPlaySessionPaused(bool bPause);
        NODISCARD bool IsEjectedFromPlay() const { return bEjectedFromPlay; }
        void SetEjectedFromPlay(bool bEject);
        void TickEjectState();

        void DrawSimulationControls(float ButtonSize);
        void DrawPauseResumeButton(const ImVec2& ButtonSize);
        void DrawEjectButton(const ImVec2& ButtonSize);

        // Shift and a left drag while simulating springs the grabbed body toward the cursor, as the physics asset editor does.
        void UpdateSimulationGrab(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize, bool bInViewportHovered);
        bool IsSimulationGrabActive() const { return GrabbedTarget.IsSet(); }

        void GroupSelectedEntities();
        void DropSelectionToFloor();

        // The selection takes the editor camera's placement, or only its rotation with bRotationOnly.
        void AlignSelectionToView(bool bRotationOnly);
        void AlignViewToSelection();

        void OpenTransformDialog();
        void ApplyTransformDialog();
        void FrameAllEntities();
        void CopyTransformFromLastSelected();
        void PasteTransformToSelection();
        void SaveCameraBookmark(int32 Slot);
        void RecallCameraBookmark(int32 Slot);
        void DrawCursorWorldPositionOverlay(ImVec2 ViewportOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera);
        void DrawEntityDebugOverlay(ImVec2 ViewportOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera);
        void DrawOffscreenSelectionIndicators(ImVec2 ViewportOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera);

    protected:

        void OnPostUndoRedo() override;

        // Undo is blocked during play, since its transactions target the editor's source world.
        bool AllowsUndoRedo() const override;

        void OnSelectionChanged() override { bDetailsDirty = true; bSelectionTransformRefreshPending = true; }
        CPackage* GetScenePackage() const override;
        NODISCARD bool SupportsSceneFolders() const override { return true; }
        void DrawOutlinerWorldSelector() override;
        void HandleOutlinerEmptyAreaDrop() override;
        void HandlePrefabContentDrop(FStringView VirtualPath, ECS::FEntity DropTarget, bool bAttachToTarget, const FTransform* SpawnTransform = nullptr) override;
        void OnDropSpawnedEntity(ECS::FEntity Entity) override;

        void SetWorldPlayInEditor(bool bShouldPlay);
        void SetWorldNewSimulate(bool bShouldSimulate);
        void DrawPlaySettingsPopup();

        // Starts the game in its own process against this project, with no editor around it.
        void LaunchStandalone();

        void RebindRegistryObservers();
        void UnbindRegistryObservers();

        // Points the outliner, details and selection at NewWorld without touching the viewport or play; null follows World.
        void SetObservedWorld(CWorld* NewWorld);

        // Drops everything tied to OldWorld and rebinds to NewWorld, keeping ProxyWorld so play can still restore the source map.
        void OnWorldTraveled(CWorld* OldWorld, CWorld* NewWorld);

        // Raised mid world tick, so it only flags and Update ends play at FrameStart.
        void OnGameQuitRequested() { bGameQuitRequested = true; }

        void HandleEntityEditorDragDrop(FTreeListView& Tree, ECS::FEntity DropItem);

        // A drop onto a folder row files entities, reparents a folder, or spawns a dropped asset into it.
        void HandleFolderDragDrop(uint32 FolderID);

        void DrawWorldSettings(bool bFocused);
        void DrawSystemsPanel(bool bFocused);
        void DrawSelectedSystemDetails(const CWorld::FSystemInfo& Info, const TVector<FSystemScheduleEntry>& Schedule);

    private:

        enum class ETransformDialogOrder : uint8 { Pre, Post };

        // One delta composed once and multiplied onto each selected entity, before or after its own transform.
        struct FTransformDialogState
        {
            FVector3                Translate = FVector3(0.0f);
            FVector3                RotateDegrees = FVector3(0.0f);
            FVector3                Scale = FVector3(1.0f);
            ETransformDialogOrder   Order = ETransformDialogOrder::Post;
        };

        struct FSelectionBox
        {
            bool    bActive = false;
            ImVec2  Start;
            ImVec2  Current;
        };

        struct FPlayInEditorSettings
        {
            int32       NumPlayers = 1;
            ENetMode    NetMode = ENetMode::Standalone;

            // A separate game process reads assets off disk, so it opens the edited map only once saved.
            bool        bStandaloneCurrentMap = true;
            bool        bStandaloneSaveFirst = true;
            bool        bStandaloneBuild = true;
        };

        void RegisterEditorActions();

        // Selection mode is always first; a new mode is added by appending to EditorModes here.
        void RegisterEditorModes();

        NODISCARD IWorldEditorMode* GetActiveMode() const;

        // Also clears any half-finished gizmo or vertex snap left by the previous mode.
        void SetActiveMode(int32 NewIndex);

        // A mode holds handles into the world it entered, so every world swap exits it first and re-enters it after.
        void ExitActiveMode();
        void EnterActiveMode();

        bool CanDuplicateEntity(ECS::FEntity Entity) const override;
        bool AllowCameraPreview() const override { return !bGameViewMode; }
        void PersistCameraPreviewScale() override;

        static constexpr int32                  MaxPlayers = 4;
        static constexpr int32                  NumCameraBookmarks = 9;

        TStrongObjectPtr<CWorld>                ProxyWorld;

        // Tracked apart from EditorEntity, so play can restore the editor world even if travel swaps it mid-session.
        ECS::FEntity                            ProxyEditorEntity = ECS::NullEntity;

        // The hidden server world FWorldManager owns for a dedicated-server play session, null otherwise.
        CWorld*                                 PIEDedicatedServerWorld = nullptr;

        // RebindToWorld swaps World without touching observers, so the connected registry is tracked to unbind that one.
        ECS::FRegistry*                         ObservedRegistry = nullptr;

        FPlayInEditorSettings                   PlaySettings;
        FOnGamePreview                          OnGamePreviewStartRequested;
        FOnGamePreview                          OnGamePreviewStopRequested;
        FDelegateHandle                         WorldTraveledHandle;
        FDelegateHandle                         GameQuitHandle;
        bool                                    bGameQuitRequested = false;
        bool                                    bGamePreviewRunning = false;
        bool                                    bSimulatingWorld = false;

        // Set by the raw event handler and consumed in Update, since stopping tears the play world down mid-dispatch otherwise.
        bool                                    bStopPlayRequested = false;

        // An eject spawns a free-fly editor camera into the running play world, which possess reverses.
        bool                                    bEjectedFromPlay = false;
        ECS::FEntity                            PossessedCameraEntity = ECS::NullEntity;
        bool                                    bRestoreGameViewOnPossess = false;

        // The game's own input and cursor modes, since the flycam's look capture rewrites the shared context while ejected.
        EInputMode                              PossessedInputMode = EInputMode::Game;
        EMouseMode                              PossessedMouseMode = EMouseMode::Normal;
        bool                                    bHasPossessedInputModes = false;

        // Held across frames, since the spring needs the attach point picked on the click rather than what the ray hits later.
        SPhysicsBodyTarget                      GrabbedTarget;
        float                                   GrabDistance = 0.0f;
        FVector3                                GrabLocalOffset = FVector3(0.0f);

        TVector<TUniquePtr<IWorldEditorMode>>   EditorModes;
        int32                                   ActiveModeIndex = 0;

        TUniquePtr<FPropertyTable>              WorldSettingsPropertyTable;
        ImGuiTextFilter                         SystemsFilter;

        // Refreshed on a timer, since gathering the list configures every system class.
        TVector<CWorld::FSystemInfo>            CachedSystemInfos;
        const CWorld*                           CachedSystemsWorld = nullptr;
        double                                  CachedSystemsTime = 0.0;
        FName                                   SelectedSystem;

        FCategorizedPicker                      ScriptAttachPicker;
        FCategorizedPicker                      ComponentAttachPicker;

        // The folder a content-browser drop files its spawned entity into, set for one drop.
        uint32                                  PendingAssetDropFolder = 0;

        FTransformDialogState                   TransformDialog;
        FSelectionBox                           SelectionBox;

        // How far a Shift drag has carried the camera, taken back off the gizmo's target.
        FVector3                                GizmoFollowOffset = FVector3(0.0f);

        FTransform                              CopiedTransform;
        bool                                    bHasCopiedTransform = false;
        FTransform                              CameraBookmarks[NumCameraBookmarks];
        bool                                    bCameraBookmarkSet[NumCameraBookmarks] = {};

        bool                                    bDrawEntityDebugInfo = false;
        bool                                    bDrawNetworkDebug = false;
        bool                                    bGameViewMode = false;
        bool                                    bSavedWorldGridEnabled = true;
        bool                                    bSavedShowComponentVisualizers = true;

        bool                                    bVertexSnapAnchorValid = false;
        bool                                    bVertexSnapApplied = false;
        FVector3                                VertexSnapAnchorLocal = FVector3(0.0f);
        FVector3                                VertexSnapTargetWorld = FVector3(0.0f);
        FVector3                                VertexSnapAnchorWorld = FVector3(0.0f);
        float                                   VertexSnapPixelRadius = 16.0f;

        // Latched until Ctrl is released once it served a command chord such as Ctrl+D, so it cannot also mean vertex snap.
        bool                                    bCtrlConsumedByChord = false;
    };
}
