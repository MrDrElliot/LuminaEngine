#pragma once

#include "AssetEditors/AssetEditorTool.h"
#include "ImGuizmo.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"

namespace Lumina
{
    class CWorld;
    class CObject;
    class CStruct;
    class CStaticMesh;
    class CPackage;
    struct FPropertyChangedEvent;

    // Shared base for ECS scene editors (world + prefab). A prefab is a mini-scene, so both
    // tools run on the same editing/play logic; subclasses add only their extras (the world
    // editor adds travel/world-settings/terrain+navmesh modes, the prefab editor adds the
    // CPrefab load/commit layer + single-root invariant).
    class FSceneEditorTool : public FAssetEditorTool
    {
        using Super = FAssetEditorTool;

    public:

        // Prefab path: a distinct asset (e.g. CPrefab) edited through a separate preview world.
        FSceneEditorTool(IEditorToolContext* Context, const FString& DisplayName, CObject* InAsset, CWorld* InWorld);

        // World path: the CWorld itself is the document/asset (forwarded as both).
        FSceneEditorTool(IEditorToolContext* Context, const FString& DisplayName, CWorld* InWorld);

        ~FSceneEditorTool() override = default;

        // Save = commit the scene back to its asset, then the standard asset save. Subclasses
        // that need extra save-time behavior (e.g. the world editor's "Save As" for an unsaved
        // transient world) override and call Super at the right point.
        void OnSave() override;

    protected:

        // Fired once when the backing asset has loaded (via FAssetEditorTool's broadcast).
        // Renamed scene-flavored hook; the prefab editor loads its preview world here.
        virtual void OnSceneLoaded() {}

        // Write the editing surface back into the backing asset. No-op for the world editor
        // (it edits the live CWorld in place); the prefab editor commits its preview world.
        virtual void CommitScene() {}

        // Mark the scene's package dirty (guards the transient no-package case).
        void MarkSceneDirty();

        // Hook: the package the scene persists into. Base = the FAssetEditorTool Asset's package
        // (prefab editor → the CPrefab). The world editor overrides it to the live CWorld's package,
        // since its world is not held as Asset.
        virtual CPackage* GetScenePackage() const;

        // Routes FAssetEditorTool's asset-loaded broadcast to OnSceneLoaded.
        void OnAssetLoadFinished() final;

        // --- Selection ------------------------------------------------------------------
        // Canonical multi-select model, mirrored on the registry as FSelectedInEditorComponent
        // (+ FLastSelectedTag for the focused entity). LastSelectedEntity is cached for O(1) reads.

        // Replace the selection with a single entity (implicit clear); the common "click" path.
        void SetSingleSelectedEntity(entt::entity Entity);
        // Add to the selection and promote to last-selected. bRebuild is vestigial.
        void AddSelectedEntity(entt::entity Entity, bool bRebuild = false);
        // Remove from the selection; picks a new last-selected if the focus was removed.
        void RemoveSelectedEntity(entt::entity Entity, bool bRebuild = false);
        // Ctrl-click semantics.
        void ToggleSelectedEntity(entt::entity Entity);
        void ClearSelectedEntities();
        // Rebuild the cached set + last-selected from the registry tags (after undo/redo/world swap).
        void ResyncSelectionFromRegistry();

        NODISCARD bool IsEntitySelected(entt::entity Entity) const { return SelectedEntities.find(Entity) != SelectedEntities.end(); }
        NODISCARD const THashSet<entt::entity>& GetSelectedEntities() const { return SelectedEntities; }
        NODISCARD entt::entity GetLastSelectedEntity() const { return LastSelectedEntity; }

        // The world whose scene the outliner/details/selection currently inspect. Defaults to the
        // tool's own World; the world editor can repoint it at another live world (e.g. a networked
        // client/server) for inspect-only viewing. Null means "follow World".
        NODISCARD CWorld* GetObservedWorld() const { return ObservedWorld != nullptr ? ObservedWorld : World.Get(); }
        // True while inspecting a world other than the tool's own (authoring/gizmo are suppressed).
        NODISCARD bool IsInspectingForeignWorld() const { return ObservedWorld != nullptr && ObservedWorld != World.Get(); }

        // The registry holding the scene's entities (the observed world's registry).
        NODISCARD FEntityRegistry& GetSceneRegistry() const { return ECS::GetWorldRegistry(*GetObservedWorld()); }

        // Outliner observes this world instead of World (null/own-world = follow World). Subclasses
        // that switch worlds set it; the base only reads it through GetObservedWorld/GetSceneRegistry.
        CWorld*                 ObservedWorld = nullptr;

        // Mirror a row's selected visual into the outliner tree.
        void SyncOutlinerRowSelection(entt::entity Entity, bool bSelected);

        // Hook: the selection focus changed. The world editor marks its details panel dirty.
        virtual void OnSelectionChanged() {}

        THashSet<entt::entity>  SelectedEntities;
        entt::entity            LastSelectedEntity = entt::null;

        // --- Scene outliner (incremental tree) -----------------------------------------
        // EntityToTreeNode maps a live entity to its row; the world drives this incrementally
        // through EnTT construct/destroy observers, the prefab through MarkTreeDirty rebuilds.
        // Both share the same node-building code, gated by IsOutlinerEntityVisible.

        struct FEntityListViewItemData
        {
            entt::entity Entity = entt::null;
        };

        // Repopulate roots (children fill lazily on expand). Wire as the tree's RebuildTreeFunction.
        void RebuildSceneOutliner(FTreeListView& Tree);
        // Add an entity row (under its parent if present). Returns the node or InvalidTreeNode.
        FTreeNodeID AddEntityToOutliner(entt::entity Entity);
        // Populate a row's rich hover tooltip (title, subtitle, component chips). Deferred to first
        // hover because the component scan is expensive; no-op once Display.bTooltipBuilt is set.
        void BuildEntityTooltip(entt::entity Entity, FTreeNodeDisplay& Display);
        void RemoveEntityFromOutliner(entt::entity Entity);
        void ReparentEntityInOutliner(entt::entity Entity);
        void RefreshOutlinerExpander(entt::entity Entity);
        // Lazily build child rows for a node on first expand. Wire as BuildChildrenFunction.
        void BuildEntityChildren(FTreeListView& Tree, FTreeNodeID Item);
        // Drain PendingOutlinerAdds before the tree draws (signal-driven path).
        void FlushOutlinerPending();

    public:
        // EnTT SNameComponent observers (signal-driven world path). Public so subclasses can
        // connect them on their registry via &FSceneEditorTool::On... (protected access would block it).
        void OnOutlinerEntityConstructed(entt::registry& Registry, entt::entity Entity);
        void OnOutlinerEntityDestroyed(entt::registry& Registry, entt::entity Entity);

    protected:

        // Hook: which entities appear in the outliner. Base = named + not FHideInSceneOutliner;
        // the prefab editor also requires SPrefabComponent so preview fixtures stay hidden.
        virtual bool IsOutlinerEntityVisible(entt::entity Entity) const;

        // The whole "Scene Graph" panel (add button + search + filter + count + tree) is shared.
        void DrawOutliner(bool bFocused);
        // Hook: extra row drawn between the search toolbar and the entity tree. The world editor
        // draws its live-world selector here when networked play has more than one world.
        virtual void DrawOutlinerWorldSelector() {}
        // Component-type filter checklist (shown in the panel's filter popup).
        void DrawFilterOptions();
        // Count of entities currently shown in the outliner (IsOutlinerEntityVisible).
        NODISCARD size_t CountOutlinerEntities() const;

        // The shared "Add" menu opened by the Scene Graph "+" button (and reusable for a focused
        // entity): empty entity, primitives, components, and instantiable prefabs. Identical in both tools.
        void DrawAddToEntityOrWorldPopup(entt::entity Entity = entt::null);
        ImGuiTextFilter AddEntityComponentFilter;

        // Hook: a content-browser asset dropped on the empty tree area (world: instantiate; prefab: spawn under root).
        virtual void HandleOutlinerEmptyAreaDrop() {}
        // Hook: instantiate/spawn a content asset (e.g. a prefab) at VirtualPath under DropTarget.
        // Used by the shared Add menu's Prefabs section and by content-browser drops.
        virtual void HandlePrefabContentDrop(FStringView VirtualPath, entt::entity DropTarget) {}

        struct FEntityListFilterState
        {
            ImGuiTextFilter FilterName;
            TVector<FName>  ComponentFilters;
        };
        FEntityListFilterState              EntityFilterState;

        // True while the Scene Graph panel is focused/hovered, so selection shortcuts work from here.
        bool                                bOutlinerActive = false;

        FTreeListView                       OutlinerListView;
        FTreeListViewContext                OutlinerContext;
        THashMap<entt::entity, FTreeNodeID> EntityToTreeNode;
        TVector<entt::entity>               PendingOutlinerAdds;

        // --- Component details / property tables ---------------------------------------
        // Canonical (world) model: one FPropertyTable row per reflected component, plus
        // multi-edit across the whole selection.

        struct FComponentDestroyRequest
        {
            const CStruct* Type = nullptr;
            entt::entity   EntityID = entt::null;
        };

        struct FComponentTableEntry
        {
            TUniquePtr<FPropertyTable> Table;
            const CStruct*             ReflectedType = nullptr;  // reflected component CStruct
            FString                    Title;                    // header label + sort key
        };

        // Rebuild PropertyTables for Entity (component intersection + multi-edit across the selection).
        void RebuildPropertyTables(entt::entity Entity);
        // Draw every component row for Entity.
        void DrawComponentList(entt::entity Entity);
        void DrawComponentHeader(FComponentTableEntry& Entry, entt::entity Entity);
        // Remove a reflected component from Entity (marks details dirty for rebuild).
        void RemoveComponent(entt::entity Entity, const CStruct* ComponentType);
        // Drain queued reflected-component removals inside one transaction. Call from Update.
        void ProcessComponentEditRequests();

        void OnPrePropertyChangeEvent(const FPropertyChangedEvent& Event);
        void OnPostPropertyChangeEvent(const FPropertyChangedEvent& Event);

        // Hook: components that should never appear in the details panel. Base hides STagComponent
        // (rendered separately as chips); the prefab editor also hides its SPrefabComponent.
        virtual bool IsComponentHiddenInDetails(const CStruct* Type) const;

        // --- Entity / component creation -----------------------------------------------

        // Create an empty entity (or one carrying Component / a primitive mesh), select it, and
        // give subclasses a post-create hook. All wrapped in one transaction.
        void CreateEntity();
        void CreateEntityWithComponent(const CStruct* Component);
        void CreatePrimitiveEntity(CStaticMesh* PrimitiveMesh, const char* DisplayName);

        // Entities a component-add targets: the whole selection when Entity is part of a
        // multi-selection, otherwise just Entity.
        TVector<entt::entity> GetComponentEditTargets(entt::entity Entity);
        // Add the picked reflected component to every target (skips ones already holding it).
        void ApplyAddComponentToTargets(const TVector<entt::entity>& Targets, entt::meta_type PickedMetaType);
        // Filterable, categorized list of addable reflected components.
        // Fills OutMetaType/OutStruct and returns true on click.
        bool DrawAddableComponentList(const ImGuiTextFilter& Filter, entt::meta_type& OutMetaType, CStruct*& OutStruct);

        // Hook: called right after a new entity is constructed in the scene (still inside the
        // create transaction). The prefab editor tags it with SPrefabComponent + parents it under root.
        virtual void OnEntityCreatedInScene(entt::entity Entity) {}
        // Hook: world transform for a newly-created entity. World = in front of the camera;
        // the prefab editor overrides to identity (it reparents under the root).
        virtual FTransform GetNewEntitySpawnTransform() const;

        // --- Clipboard + visualizers ---------------------------------------------------

        // Entity clipboard (mirrored on the registry as FCopiedTag).
        void AddEntityToCopies(entt::entity Entity);
        void RemoveEntityFromCopies(entt::entity Entity);
        void ClearCopies() const;
        // Deep-copy From into a new entity To (component duplicate via the editor's default filter).
        void CopyEntity(entt::entity& To, entt::entity From);

        // Draws component visualizers for the current selection (+ their children). Shared EndFrame body.
        void EndFrame() override;

        bool bShowComponentVisualizers = true;

        // --- Gizmo state (shared) ------------------------------------------------------
        // Manipulation happens in each tool's DrawViewportOverlayElements (world adds editor-mode
        // gating + vertex-snap); the operation/space/snap state and the cycle/toggle helpers are shared.
        void CycleGuizmoOp();
        void ToggleGuizmoMode();

        // Hook: persist the current gizmo snap members to the tool's settings object. Default no-op;
        // World/Prefab editors override to write their CDeveloperSettings + save.
        virtual void PersistGizmoSettings() {}
        
        void DrawViewportToolbar(const FUpdateContext& UpdateContext) override;
        void DrawCameraControls(float ButtonSize);
        void DrawViewportOptions(float ButtonSize);
        void DrawSnapSettingsPopup();

        // Hook: true while the viewport is showing a running/simulating game (shrinks the bar).
        virtual bool IsViewportPlaying() const { return false; }
        // Hook: leading play/simulate controls (+ its own trailing separator). World only.
        virtual void DrawViewportToolbarPlayControls(float ButtonSize) {}
        // Hook: trailing editor-mode selector + active-mode toolbar (+ its own leading separator). World only.
        virtual void DrawViewportToolbarModeSelector(float ButtonSize) {}
        // Hook: extra items at the bottom of the View Mode popup (world: Game View, Entity Debug Info).
        virtual void DrawViewModeExtraItems() {}

        ImGuizmo::OPERATION GuizmoOp = ImGuizmo::TRANSLATE;
        ImGuizmo::MODE      GuizmoMode = ImGuizmo::WORLD;
        

        // Tool-window body: resolves the last-selected entity, rebuilds tables, draws the panel.
        void DrawDetailsPanel(bool bFocused);
        // The panel header (name + add-component + delete) and component list for one entity.
        void DrawEntityProperties(entt::entity Entity);
        void DrawEmptyState();

        // Hook: whether Entity may be deleted from the panel (prefab forbids the root).
        virtual bool CanDeleteEntity(entt::entity Entity) const { return true; }
        // Hook: extra header buttons next to Add-Component/Delete (world: Add Tag).
        virtual void DrawDetailsHeaderExtraButtons(entt::entity Entity) {}
        // Hook: extra sections above the component list (world: the Tags chip section).
        virtual void DrawDetailsExtraSections(entt::entity Entity) {}

        TQueue<entt::entity> EntityDestroyRequests;
        bool                bImGuizmoUsedOnce = false;
        bool                bGuizmoSnapEnabled = true;
        float               GuizmoSnapTranslate = 0.1f;
        float               GuizmoSnapRotate = 5.0f;
        float               GuizmoSnapScale = 0.1f;

        TVector<FComponentTableEntry>    PropertyTables;
        TQueue<FComponentDestroyRequest> ComponentDestroyRequests;
        entt::entity                     DetailsEntity = entt::null;
        bool                             bDetailsDirty = false;
    };
}
