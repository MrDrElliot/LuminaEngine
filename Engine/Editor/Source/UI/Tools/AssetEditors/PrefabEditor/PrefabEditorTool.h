#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"
#include "Memory/SmartPtr.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/Tools/FSceneEditorTool.h"
#include "World/ECS/Registry.h"

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

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnSceneLoaded() override;
        void CommitScene() override;
        void OnSave() override;
        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return true; }

        // A variant gets the branch icon, so the tab says what it is without being opened.
        const char* GetTitlebarIcon() const override;

        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        void PersistGizmoSettings() override;
        void PersistCameraPreviewScale() override;

    protected:

        void OnPostUndoRedo() override;

    private:

        void HandleOutlinerEmptyAreaDrop() override;
        void HandlePrefabContentDrop(FStringView VirtualPath, ECS::FEntity DropTarget, bool bAttachToTarget, const FTransform* SpawnTransform = nullptr) override;

        // The root cannot be deleted or duplicated, and the internal SPrefabComponent stays out of the details.
        bool CanDeleteEntity(ECS::FEntity Entity) const override;
        bool CanDuplicateEntity(ECS::FEntity Entity) const override { return CanDeleteEntity(Entity); }
        bool IsComponentHiddenInDetails(const CStruct* Type) const override;

        // Only prefab-owned entities show, which hides the preview lights, floor and camera.
        bool IsOutlinerEntityVisible(ECS::FEntity Entity) const override;

        // New entities are tagged as the prefab's own, parented under the root, and spawned at identity.
        void OnEntityCreatedInScene(ECS::FEntity Entity) override;
        ECS::FEntity DuplicateEntityForScene(ECS::FEntity Source) override { return DuplicatePrefabEntity(Source); }
        void AdoptSpawnedSubtree(ECS::FEntity Root) override;
        FString GetReparentRefusal(ECS::FEntity Entity, ECS::FEntity NewParent) override;
        FTransform GetNewEntitySpawnTransform() const override;

        // Names the parent above the Scene Graph when this prefab is a variant.
        void DrawVariantBanner();

        void HandleOutlinerDragDrop(FTreeListView& Tree, ECS::FEntity DropItem);

        // Tags one entity as this prefab's own and strips the instance tracking a dropped prefab brings.
        void AdoptIntoPrefab(ECS::FEntity Entity);

        void RequestDestroyEntity(ECS::FEntity Entity);
        void ProcessDestroyRequests();
        void ResetSelectionTransform();
        ECS::FEntity DuplicatePrefabEntity(ECS::FEntity Source);
        void ProcessClipboardShortcuts();
        void FrameAllEntities();
        void LoadPrefabIntoPreviewWorld();
        void CommitPreviewWorldToPrefab();

        // A reload passes bResetToAuto, so a prefab that lights itself starts without the studio rig doubling its light.
        void SyncPreviewLighting(bool bResetToAuto);

        ECS::FEntity FindPrefabRoot() const;
        CPrefab* GetPrefab() const;
        void RegisterEditorActions();

        // Match the world editor's window names, so both editors read the same.
        static constexpr const char*        OutlinerWindowName = "Scene Graph";
        static constexpr const char*        PropertiesWindowName = "Details";

        ECS::FEntity                        DirectionalLightEntity = ECS::NullEntity;

        // Derived on load, then the user's to toggle.
        bool                                bStudioLighting = true;
        bool                                bPrefabSuppliesLighting = false;

        // Several prefab entities can go down in one batch, so the package is marked dirty once for all of them.
        bool                                bPendingDirtyOnDestroy = false;

        // The resolve count the preview world was built from, where a mismatch means a parent edit landed.
        uint32                              LastVariantResolveCount = 0;
        bool                                bParentChangeDeferred = false;

        // The prefab's own ParentPrefab slot above the Scene Graph, built on first use.
        TUniquePtr<FPropertyTable>          VariantPropertyTable;
    };
}
