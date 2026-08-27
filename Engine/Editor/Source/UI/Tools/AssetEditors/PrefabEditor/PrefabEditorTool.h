#pragma once

#include "World/ECS/Registry.h"


#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/Tools/FSceneEditorTool.h"
#include "Memory/SmartPtr.h"


namespace Lumina
{
    class CPrefab;
    class CStaticMesh;
    class CStruct;

    class EDITOR_API FPrefabEditorTool : public FSceneEditorTool
    {
        using Super = FSceneEditorTool;

    public:

        LUMINA_EDITOR_TOOL(FPrefabEditorTool)

        FPrefabEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }

        // A variant gets the branch icon so the tab says what it is without being opened.
        const char* GetTitlebarIcon() const override;
        void OnInitialize() override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void OnSceneLoaded() override;
        void CommitScene() override;
        void OnSave() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        // Viewport overlay toolbar is shared (FSceneEditorTool); the prefab persists snap to its own settings.
        void PersistGizmoSettings() override;
        void PersistCameraPreviewScale() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        bool ShouldGenerateThumbnailOnSave() const override { return true; }

        bool bShowAABB = false;

        // GuizmoOp/GuizmoMode + snap state now live in FSceneEditorTool.
        ECS::FEntity DirectionalLightEntity = ECS::NullEntity;

        /** Whether the preview studio rig is active. Auto-derived on load, then user-owned. */
        bool bStudioLighting = true;

        /** Set when the loaded prefab carries its own directional light, environment or skylight. */
        bool bPrefabSuppliesLighting = false;

    protected:

        void OnPostUndoRedo() override;

    private:

        // The Scene Graph panel + shared Add menu live in FSceneEditorTool; the prefab supplies the
        // empty-area asset drop (spawns under the root) and the prefab-instantiation hook.
        void HandleOutlinerEmptyAreaDrop() override;

        // The details panel is shared (FSceneEditorTool::DrawDetailsPanel); the prefab only guards
        // root deletion and hides its internal SPrefabComponent.
        bool CanDeleteEntity(ECS::FEntity Entity) const override;
        bool IsComponentHiddenInDetails(const CStruct* Type) const override;

        // Restrict the shared outliner to prefab-owned entities (hides preview lights/floor/camera).
        bool IsOutlinerEntityVisible(ECS::FEntity Entity) const override;

        // Header above the Scene Graph naming the parent, when this prefab is a variant.
        void DrawVariantBanner();

        void HandleOutlinerDragDrop(FTreeListView& Tree, ECS::FEntity DropItem);
        void HandlePrefabContentDrop(FStringView VirtualPath, ECS::FEntity DropTarget, bool bAttachToTarget) override;

        // Base CreateEntity*/component-add path; the prefab supplies these hooks: tag new entities
        // with SPrefabComponent + parent them under the root, and spawn at identity (not the camera).
        void OnEntityCreatedInScene(ECS::FEntity Entity) override;
        FTransform GetNewEntitySpawnTransform() const override;

        void RequestDestroyEntity(ECS::FEntity Entity);
        void ProcessDestroyRequests();
        void ResetSelectionTransform();

        // Hierarchy / multi-select shortcuts.
        ECS::FEntity DuplicatePrefabEntity(ECS::FEntity Source);
        void ProcessClipboardShortcuts();

        // Selection model (SetSingleSelectedEntity/Add/Remove/Toggle/Clear/Resync, the cached
        // SelectedEntities + LastSelectedEntity) now lives in FSceneEditorTool.

        // View / camera helpers.
        void FrameAllEntities();

        // Prefab world setup.
        void LoadPrefabIntoPreviewWorld();
        void CommitPreviewWorldToPrefab();

        /**
         * Adds or removes the preview studio rig to match bStudioLighting, and recomputes whether the
         * prefab lights itself. Pass bResetToAuto on a (re)load to re-derive the default: a prefab that
         * ships its own lighting starts with the rig off, since stacking the two double-lights the scene
         * and leaves two singleton environments racing for the frame.
         */
        void SyncPreviewLighting(bool bResetToAuto);

        ECS::FEntity FindPrefabRoot() const;

        CPrefab* GetPrefab() const;

        // Editor actions registration (W/E/R/X gizmo, Ctrl+Z/Y undo/redo, F focus, Home frame all,
        // Ctrl+S save). Must be called from OnInitialize.
        void RegisterEditorActions();

    private:

        // Component property tables + the Scene Graph panel/engine (OutlinerListView/OutlinerContext/
        // EntityToTreeNode/EntityFilterState) and EntityDestroyRequests now live in FSceneEditorTool.

        // Track when a prefab-owning entity is destroyed mid-frame so we can mark the package
        // dirty exactly once even when several entities go down in the same batch.
        bool                                bPendingDirtyOnDestroy = false;

        // Resolve count the preview world was built from; a mismatch means a parent edit landed.
        uint32                              LastVariantResolveCount = 0;

        // Draws the prefab's own ParentPrefab slot above the Scene Graph. Built on first use.
        TUniquePtr<FPropertyTable>          VariantPropertyTable;

        // Match the world editor's window names so both editors read the same.
        static constexpr const char* OutlinerWindowName = "Scene Graph";
        static constexpr const char* PropertiesWindowName = "Details";
    };
}
