#pragma once

#include "World/ECS/Registry.h"


#include "Containers/Queue.h"
#include "AssetEditors/AssetEditorTool.h"
#include "ImGuizmo.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/Properties/PropertyEditContexts.h"

namespace Lumina
{
    class CWorld;
    class CObject;
    class CStruct;
    class CStaticMesh;
    class CPackage;
    class IRenderScene;
    struct SSceneFolderComponent;
    struct FPropertyChangedEvent;

    class EDITOR_API FSceneEditorTool : public FAssetEditorTool
    {
        using Super = FAssetEditorTool;

    public:

        // Prefab path: a distinct asset (e.g. CPrefab) edited through a separate preview world.
        FSceneEditorTool(IEditorToolContext* Context, const FString& DisplayName, CObject* InAsset, CWorld* InWorld);

        // World path: the CWorld itself is the document/asset (forwarded as both).
        FSceneEditorTool(IEditorToolContext* Context, const FString& DisplayName, CWorld* InWorld);

        ~FSceneEditorTool() override = default;
        
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
        
        virtual CPackage* GetScenePackage() const;

        // Routes FAssetEditorTool's asset-loaded broadcast to OnSceneLoaded.
        void OnAssetLoadFinished() final;
        

        // Replace the selection with a single entity (implicit clear); the common "click" path.
        void SetSingleSelectedEntity(ECS::FEntity Entity);
        // Add to the selection and promote to last-selected. bRebuild is vestigial.
        void AddSelectedEntity(ECS::FEntity Entity, bool bRebuild = false);
        // Remove from the selection; picks a new last-selected if the focus was removed.
        void RemoveSelectedEntity(ECS::FEntity Entity, bool bRebuild = false);
        // Ctrl-click semantics.
        void ToggleSelectedEntity(ECS::FEntity Entity);
        void ClearSelectedEntities();

        // Drops the selection and its mirror tags, null-safe because teardown runs after the world is gone.
        void ResetSelectionState();
        // Rebuild the cached set + last-selected from the registry tags (after undo/redo/world swap).
        void ResyncSelectionFromRegistry();

        // Re-stamp the non-serialized selection tags from the surviving CPU set (a true-restore keeps handles but drops tags).
        void ReapplySelectionTags();

        NODISCARD bool IsEntitySelected(ECS::FEntity Entity) const { return SelectedEntities.find(Entity) != SelectedEntities.end(); }
        NODISCARD const THashSet<ECS::FEntity>& GetSelectedEntities() const { return SelectedEntities; }
        NODISCARD ECS::FEntity GetLastSelectedEntity() const { return LastSelectedEntity; }

        // The world whose scene the outliner/details/selection currently inspect. Defaults to the
        // tool's own World; the world editor can repoint it at another live world (e.g. a networked
        // client/server) for inspect-only viewing. Null means "follow World".
        NODISCARD CWorld* GetObservedWorld() const { return ObservedWorld != nullptr ? ObservedWorld : World.Get(); }
        // True while inspecting a world other than the tool's own (authoring/gizmo are suppressed).
        NODISCARD bool IsInspectingForeignWorld() const { return ObservedWorld != nullptr && ObservedWorld != World.Get(); }

        // The registry holding the scene's entities (the observed world's registry).
        NODISCARD ECS::FRegistry& GetSceneRegistry() const { return ECS::GetWorldRegistry(*GetObservedWorld()); }

    public:

        // Read access to the scene this tool is showing, for callers outside it such as an agent endpoint.
        NODISCARD CWorld* GetSceneWorld() const { return GetObservedWorld(); }

        NODISCARD ECS::FRegistry& GetSceneEntityRegistry() const { return GetSceneRegistry(); }

        // Brackets a mutation from outside the tool, so an agent edit lands on the same undo stack.
        void RunTransacted(FName Label, const TFunction<void()>& Mutate);

        // The same for an edit that only adds entities, which records far less than a full snapshot.
        void RunCreationTransacted(FName Label, const TFunction<void()>& Mutate);

    protected:


        // Outliner observes this world instead of World (null/own-world = follow World). Subclasses
        // that switch worlds set it; the base only reads it through GetObservedWorld/GetSceneRegistry.
        CWorld*                 ObservedWorld = nullptr;

        // Mirror a row's selected visual into the outliner tree.
        void SyncOutlinerRowSelection(ECS::FEntity Entity, bool bSelected);

    public:

        // Scrolls the outliner to Entity's row.
        void RevealEntityInOutliner(ECS::FEntity Entity);

    protected:

        // Hook: the selection focus changed. The world editor marks its details panel dirty.
        virtual void OnSelectionChanged() {}

        THashSet<ECS::FEntity>  SelectedEntities;
        ECS::FEntity            LastSelectedEntity = ECS::NullEntity;

        struct FEntityListViewItemData
        {
            ECS::FEntity Entity = ECS::NullEntity;
            // Non-zero on a folder row, in which case Entity is null.
            uint32       FolderID = 0;
        };

        // Repopulate roots (children fill lazily on expand). Wire as the tree's RebuildTreeFunction.
        void RebuildSceneOutliner(FTreeListView& Tree);
        // Add an entity row (under its parent if present). Returns the node or InvalidTreeNode.
        FTreeNodeID AddEntityToOutliner(ECS::FEntity Entity);
        // Populate a row's rich hover tooltip (title, subtitle, component chips). Deferred to first
        // hover because the component scan is expensive; no-op once Display.bTooltipBuilt is set.
        void BuildEntityTooltip(ECS::FEntity Entity, FTreeNodeDisplay& Display);
        void RemoveEntityFromOutliner(ECS::FEntity Entity);
        void ReparentEntityInOutliner(ECS::FEntity Entity);
        void RefreshOutlinerExpander(ECS::FEntity Entity);
        // Lazily build child rows for a node on first expand. Wire as BuildChildrenFunction.
        void BuildEntityChildren(FTreeListView& Tree, FTreeNodeID Item);
        // Drain PendingOutlinerAdds before the tree draws (signal-driven path).
        void FlushOutlinerPending();

    public:
        // SNameComponent observers (signal-driven world path). Public so subclasses can
        // connect them on their registry via &FSceneEditorTool::On... (protected access would block it).
        void OnOutlinerEntityConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnOutlinerEntityDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);

    protected:

        // Outliner folders. The table lives on the world's singleton entity, which the outliner hides.

        // Hook: does this tool offer folders? Only the world editor does; the prefab editor's rows stay entities.
        NODISCARD virtual bool SupportsSceneFolders() const { return false; }

        // The observed world's folder table for reading; null when there is none (or folders are unsupported).
        NODISCARD const SSceneFolderComponent* GetSceneFolders() const;
        // Same table for writing. Null while inspecting a foreign world, or before the first folder exists.
        NODISCARD SSceneFolderComponent* GetEditableSceneFolders() const;

        NODISCARD uint32 GetEntityFolderID(ECS::FEntity Entity) const;
        NODISCARD FTreeNodeID FindFolderNode(uint32 FolderID) const;
        // "Lighting/Sky" style path, for menu labels.
        NODISCARD FString GetFolderPath(uint32 FolderID) const;
        NODISCARD FName MakeUniqueFolderName(uint32 ParentID) const;

        // Materialize every folder row and refresh the entity->folder cache. Runs before the entity rows.
        void BuildFolderNodes(FTreeListView& Tree);
        // Entities filed under a folder, optionally including its subfolders'.
        void CollectFolderEntities(uint32 FolderID, TVector<ECS::FEntity>& OutEntities, bool bRecursive) const;

        uint32 CreateSceneFolder(const FName& Name, uint32 ParentID);
        void RenameSceneFolder(uint32 FolderID, FStringView NewName);
        void DeleteSceneFolder(uint32 FolderID, bool bDeleteContents);
        void MoveFolderIntoFolder(uint32 FolderID, uint32 NewParentID);
        // Files entities under FolderID (0 unfiles them), detaching any that are parented to another entity.
        void MoveEntitiesToFolder(const TVector<ECS::FEntity>& Entities, uint32 FolderID);
        void SelectFolderContents(uint32 FolderID);
        void SetFolderContentsHidden(uint32 FolderID, bool bHidden);

        // Context-menu bodies, shared by both outliner menus.
        void DrawFolderContextMenu(uint32 FolderID);
        void DrawMoveToFolderMenuItems(const TVector<ECS::FEntity>& Entities);
        // "New Folder" toolbar button next to the Scene Graph "+".
        void DrawNewFolderButton(float ButtonSize);

        THashMap<uint32, FTreeNodeID>  FolderToTreeNode;
        THashMap<ECS::FEntity, uint32> EntityFolderCache;
        // Folder whose row should enter inline rename on the next draw (a freshly created one).
        uint32                         PendingFolderRename = 0;

        // Hook: which entities appear in the outliner. Base = named + not FHideInSceneOutliner;
        // the prefab editor also requires SPrefabComponent so preview fixtures stay hidden.
        virtual bool IsOutlinerEntityVisible(ECS::FEntity Entity) const;

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
        void DrawAddToEntityOrWorldPopup(ECS::FEntity Entity = ECS::NullEntity);
        ImGuiTextFilter AddEntityComponentFilter;

        virtual void HandleOutlinerEmptyAreaDrop() {}
        virtual void HandlePrefabContentDrop(FStringView VirtualPath, ECS::FEntity DropTarget, bool bAttachToTarget) {}

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
        THashMap<ECS::FEntity, FTreeNodeID> EntityToTreeNode;
        TVector<ECS::FEntity>               PendingOutlinerAdds;
        TVector<ECS::FEntity>               PendingExpanderRefresh;

        struct FComponentDestroyRequest
        {
            const CStruct* Type = nullptr;
            ECS::FEntity   EntityID = ECS::NullEntity;
        };

        struct FComponentTableEntry
        {
            TUniquePtr<FPropertyTable> Table;
            const CStruct*             ReflectedType = nullptr;  // reflected component CStruct
            FString                    Title;                    // header label + sort key
        };

        // Rebuild PropertyTables for Entity (component intersection + multi-edit across the selection).
        void RebuildPropertyTables(ECS::FEntity Entity);
        // Draw every component row for Entity.
        void DrawComponentList(ECS::FEntity Entity);

        // One search box over the whole details panel. Matches a component by its title, and otherwise
        // filters INTO each component's property table, so searching a property name shows only the
        // components that actually have it.
        void DrawComponentSearchBar();

        ImGuiTextFilter DetailsFilter;
        void DrawComponentHeader(FComponentTableEntry& Entry, ECS::FEntity Entity);
        // Sockets/bones on Entity's parent's mesh, for SocketPicker FName properties in the details.
        void BuildSocketPickerData(ECS::FEntity Entity, FSocketEditContext& Out);

        // Provided to every details table this tool draws, refreshed per entity as the panel walks them.
        FPropertyEditContext  PropertyContext;
        FAnimGraphEditContext AnimGraphCtx;
        FSocketEditContext    SocketCtx;
        FWorldEditContext    WorldCtx;
        FEntityPickContext   PickCtx;
        // Remove a reflected component from Entity (marks details dirty for rebuild).
        void RemoveComponent(ECS::FEntity Entity, const CStruct* ComponentType);
        // Drain queued reflected-component removals inside one transaction. Call from Update.
        void ProcessComponentEditRequests();

        void OnPrePropertyChangeEvent(const FPropertyChangedEvent& Event);
        void OnPostPropertyChangeEvent(const FPropertyChangedEvent& Event);

        // Opens a transaction over just the component the details panel is about to write.
        void BeginPropertyEditTransaction(CStruct* ComponentType);

        // Opens a transaction over just the folder table; for edits that touch nothing else.
        void BeginFolderTransaction();

        // Adds the folder table to an open transaction, for an edit that also rewires the hierarchy.
        void RecordSceneFolderSnapshot();

        // Adds the prefab override ledger to an open transaction, for a component add or remove.
        void RecordPrefabOverrideSnapshot(const TVector<ECS::FEntity>& Entities);

        // The singleton entity carrying SSceneFolderComponent, or null before the world has one.
        NODISCARD ECS::FEntity GetSceneFolderOwner() const;

        // Hook: components that should never appear in the details panel. Base hides STagComponent
        // (rendered separately as chips); the prefab editor also hides its SPrefabComponent.
        virtual bool IsComponentHiddenInDetails(const CStruct* Type) const;
        
        // Wraps the transaction, the creation hook, selection and dirty marking that every spawn shares.
        ECS::FEntity SpawnEntityTransacted(FName Name, FName TransactionLabel,
            const TFunction<void(ECS::FEntity)>& Decorate);

        void CreateEntity();
        void CreateEntityWithComponent(const CStruct* Component);
        void CreatePrimitiveEntity(CStaticMesh* PrimitiveMesh, const char* DisplayName);

        // Entities a component-add targets: the whole selection when Entity is part of a
        // multi-selection, otherwise just Entity.
        TVector<ECS::FEntity> GetComponentEditTargets(ECS::FEntity Entity);
        // Add the picked reflected component to every target (skips ones already holding it).
        void ApplyAddComponentToTargets(const TVector<ECS::FEntity>& Targets, CStruct* PickedMetaType);
        // Filterable, categorized list of addable reflected components.
        // Fills OutMetaType/OutStruct and returns true on click.
        bool DrawAddableComponentList(const ImGuiTextFilter& Filter, const TVector<ECS::FEntity>& Targets, CStruct*& OutMetaType, CStruct*& OutStruct);
        
        virtual void OnEntityCreatedInScene(ECS::FEntity Entity) {}
        virtual FTransform GetNewEntitySpawnTransform() const;
        
        // Entity clipboard (mirrored on the registry as FCopiedTag).
        void AddEntityToCopies(ECS::FEntity Entity);
        void RemoveEntityFromCopies(ECS::FEntity Entity);
        void ClearCopies() const;
        // Deep-copy From into a new entity To (component duplicate via the editor's default filter).
        void CopyEntity(ECS::FEntity& To, ECS::FEntity From);

        // Draws component visualizers for the current selection (+ their children). Shared EndFrame body.
        void EndFrame() override;

        void CycleGuizmoOp();
        void ToggleGuizmoMode();

        // Hook: persist the current gizmo snap members to the tool's settings object. Default no-op;
        // World/Prefab editors override to write their CDeveloperSettings + save.
        virtual void PersistGizmoSettings() {}
        
        void DrawViewportToolbar(const FUpdateContext& UpdateContext) override;

        // The scene viewport toolbar carries the eye button, so the menu-bar "Visualize" dropdown stays off.
        NODISCARD bool DrawsViewModeInViewportToolbar() const override { return true; }
        void DrawCameraControls(float ButtonSize);
        void DrawViewportOptions(float ButtonSize);
        void DrawSnapSettingsPopup();

        // Hook: true while the viewport is showing a running/simulating game (shrinks the bar).
        virtual bool IsViewportPlaying() const { return false; }
        // Hook: leading play/simulate controls (+ its own trailing separator). World only.
        virtual void DrawViewportToolbarPlayControls(float ButtonSize) {}
        // Hook: trailing editor-mode selector + active-mode toolbar (+ its own leading separator). World only.
        virtual void DrawViewportToolbarModeSelector(float ButtonSize) {}

        ImGuizmo::OPERATION GuizmoOp = ImGuizmo::TRANSLATE;
        ImGuizmo::MODE      GuizmoMode = ImGuizmo::WORLD;

        void UpdateCameraPreview();
        void DrawCameraPreviewOverlay(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize);

        // Hook: extra gate for the preview (world editor: off during game view).
        virtual bool AllowCameraPreview() const { return true; }
        // Hook: persist CameraPreviewScale after a resize drag. Default no-op; World/Prefab
        // editors write their CDeveloperSettings + save (mirrors PersistGizmoSettings).
        virtual void PersistCameraPreviewScale() {}

        IRenderScene*           CameraPreviewScene = nullptr;
        int32                   CameraPreviewHandle = -1;
        bool                    bCameraPreviewActive = false;
        static constexpr uint32 CameraPreviewWidth  = 720;
        static constexpr uint32 CameraPreviewHeight = 405;

        // Preview overlay display scale + corner drag-resize state. bCameraPreviewMouseOver is
        // last frame's grip hover/drag; it gates viewport picking so a resize drag never falls
        // through to selection.
        float                   CameraPreviewScale = 0.6f;
        bool                    bCameraPreviewResizing = false;
        bool                    bCameraPreviewMouseOver = false;



        // Tool-window body: resolves the last-selected entity, rebuilds tables, draws the panel.
        void DrawDetailsPanel(bool bFocused);
        // The panel header (name + add-component + delete) and component list for one entity.
        void DrawEntityProperties(ECS::FEntity Entity);
        void DrawEmptyState();

        // Hook: whether Entity may be deleted from the panel (prefab forbids the root).
        virtual bool CanDeleteEntity(ECS::FEntity Entity) const { return true; }
        // Hook: extra header buttons next to Add-Component/Delete (world: Add Tag).
        // Modal name prompt for one entity. Also reached from the outliner context menu.
        void PushRenameEntityModal(ECS::FEntity Entity);

        // Owned by the tool rather than the modal lambda, which cannot hold state across frames.
        FFixedString RenameModalBuffer;

        virtual void DrawDetailsHeaderExtraButtons(ECS::FEntity Entity) {}
        // Hook: extra sections above the component list (world: the Tags chip section).
        virtual void DrawDetailsExtraSections(ECS::FEntity Entity) {}

        TQueue<ECS::FEntity> EntityDestroyRequests;
        bool                bImGuizmoUsedOnce = false;
        bool                bGuizmoSnapEnabled = true;
        float               GuizmoSnapTranslate = 0.1f;
        float               GuizmoSnapRotate = 5.0f;
        float               GuizmoSnapScale = 0.1f;

        TVector<FComponentTableEntry>    PropertyTables;
        TQueue<FComponentDestroyRequest> ComponentDestroyRequests;
        ECS::FEntity                     DetailsEntity = ECS::NullEntity;
        bool                             bDetailsDirty = false;

        // One-shot: arm a transform resolve for the newly selected entities on the next tick. Set from
        // OnSelectionChanged (which every selection mutator already calls), consumed and cleared by the
        // tool's tick. Selection used to arm this EVERY frame, which is O(subtree) per frame -- see the
        // consuming site in WorldEditorTool.
        bool                             bSelectionTransformRefreshPending = false;

        // Script generation the cached tables were built against. A table holds a raw pointer into a
        // component's value buffer and a CStruct* layout; a C# reload frees that buffer and destroys the
        // layout, so every table built before the bump describes memory that no longer exists.
        int32                            DetailsScriptGeneration = -1;

        // Same contract for prefab data: a refresh rewrites an instance's component set and a re-capture
        // replaces the prefab registry the reset-to-prefab baselines point into.
        uint32                           DetailsPrefabGeneration = 0;
    };
}
