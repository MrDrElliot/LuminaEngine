#pragma once

#include "AssetEditors/AssetEditorTool.h"
#include "Containers/Queue.h"
#include "ImGuizmo.h"
#include "Tools/ComponentVisualizers/ComponentVisualizerContext.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/Properties/PropertyEditContexts.h"
#include "World/ECS/Registry.h"

namespace Lumina::DragDrop
{
    struct FPayload;
}

namespace Lumina
{
    class CComponentVisualizer;
    class CObject;
    class CPackage;
    class CStaticMesh;
    class CStruct;
    class CWorld;
    class IRenderScene;
    struct FPropertyChangedEvent;
    struct SSceneFolderComponent;
    struct SStaticMeshComponent;

    class EDITOR_API FSceneEditorTool : public FAssetEditorTool, public IComponentVisualizerHost
    {
        using Super = FAssetEditorTool;

    public:

        // The prefab path, editing a separate asset through a preview world.
        FSceneEditorTool(IEditorToolContext* Context, const FString& DisplayName, CObject* InAsset, CWorld* InWorld);

        // The world path, where the world itself is the document.
        FSceneEditorTool(IEditorToolContext* Context, const FString& DisplayName, CWorld* InWorld);

        ~FSceneEditorTool() override = default;

        NODISCARD FSceneEditorTool* AsSceneEditor() override { return this; }
        void OnScriptsReloaded(const Scripting::FScriptReload& Reload) override;
        void OnSave() override;

        NODISCARD CWorld* GetSceneWorld() const { return GetObservedWorld(); }
        NODISCARD ECS::FRegistry& GetSceneEntityRegistry() const { return GetSceneRegistry(); }

        // Mutations from outside the tool, so an agent edit lands on the same undo stack as a panel edit.
        void RunTransacted(FName Label, const TFunction<void()>& Mutate);
        void RunCreationTransacted(FName Label, const TFunction<void()>& Mutate);
        void RunDestroyTransacted(FName Label, const TVector<ECS::FEntity>& Doomed, const TFunction<void()>& Mutate);
        void RunComponentTransacted(FName Label, ECS::FEntity Entity, CStruct* ComponentType, const TFunction<void()>& Mutate);
        void RemoveComponentTransacted(FName Label, ECS::FEntity Entity, CStruct* ComponentType);
        void AddComponentTransacted(const TVector<ECS::FEntity>& Targets, CStruct* ComponentType) { ApplyAddComponentToTargets(Targets, ComponentType); }

        // Gives an entity made outside SpawnEntityTransacted the same setup, such as prefab membership.
        void AdoptCreatedEntity(ECS::FEntity Entity) { OnEntityCreatedInScene(Entity); }

        // Makes a spawned subtree belong to this scene as a content drop would; the prefab editor copies it in unlinked.
        virtual void AdoptSpawnedSubtree(ECS::FEntity Root) {}

        // A copy of Source and its children as Duplicate makes it, such as with fresh prefab stable IDs.
        virtual ECS::FEntity DuplicateEntityForScene(ECS::FEntity Source);
        virtual bool CanDuplicateEntity(ECS::FEntity Entity) const { return true; }
        void DuplicateAndSelect(TSpan<const ECS::FEntity> Entities, FName Label = "Duplicate");

        // Alt pressed on the gizmo copies the selection first, so the drag moves the copies; true while Alt holds the gizmo.
        bool HandleAltDragCopy(bool bInputTaken);

        void SelectEntities(const TVector<ECS::FEntity>& Entities);
        void RevealEntityInOutliner(ECS::FEntity Entity);

        // Why Entity cannot move under NewParent, or empty when it can; a null parent means the top level.
        NODISCARD virtual FString GetReparentRefusal(ECS::FEntity Entity, ECS::FEntity NewParent);
        bool ReparentEntityTransacted(ECS::FEntity Entity, ECS::FEntity NewParent, bool bKeepWorldTransform = true, FString* OutRefusal = nullptr);

        // Called from the viewport overlay once the gizmo has had the mouse.
        void DrawComponentVisualizerOverlay(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize, const SCameraComponent& Camera, bool bAllowInput);
        NODISCARD bool AreVisualizersCapturingInput() const { return VisualizerInteraction.bCapturedInput; }
        NODISCARD bool HasVisualizerSubSelection() const;
        void BeginVisualizerTransaction(ECS::FEntity Entity, CStruct* ComponentType) override;
        void EndVisualizerTransaction(FName Label) override;
        void MarkVisualizerSceneDirty() override;

        // Public so a subclass can bind them on its registry through &FSceneEditorTool::On..., which protected access blocks.
        void OnOutlinerEntityConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnOutlinerEntityDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);

    protected:

        struct FVisualizerBinding
        {
            ECS::FSparseSet*        Storage = nullptr;
            CComponentVisualizer*   Visualizer = nullptr;
            CStruct*                ComponentType = nullptr;
        };

        struct FEntityListViewItemData
        {
            ECS::FEntity    Entity = ECS::NullEntity;
            uint32          FolderID = 0;
        };

        struct FEntityListFilterState
        {
            ImGuiTextFilter FilterName;
            TVector<FName>  ComponentFilters;
        };

        struct FComponentDestroyRequest
        {
            const CStruct*  Type = nullptr;
            ECS::FEntity    EntityID = ECS::NullEntity;
        };

        // A table edits component memory in place, so the instances it was built over are kept to tell when they move.
        struct FComponentTableEntry
        {
            TUniquePtr<FPropertyTable>  Table;
            const CStruct*              ReflectedType = nullptr;
            FString                     Title;
            void*                       Instance = nullptr;
            TVector<void*>              OtherInstances;
            float                       LastDrawnHeight = 0.0f;
        };

        virtual void OnSceneLoaded() {}

        // Writes the editing surface back into the asset; the world editor edits in place, the prefab editor commits its preview world.
        virtual void CommitScene() {}

        virtual CPackage* GetScenePackage() const;
        virtual void OnSelectionChanged() {}
        NODISCARD virtual bool SupportsSceneFolders() const { return false; }

        // Base shows named entities not tagged FHideInSceneOutliner; the prefab editor also requires prefab membership.
        virtual bool IsOutlinerEntityVisible(ECS::FEntity Entity) const;

        // A row between the outliner's search bar and its tree, where the world editor picks a networked world.
        virtual void DrawOutlinerWorldSelector() {}

        virtual void HandleOutlinerEmptyAreaDrop() {}
        virtual void HandlePrefabContentDrop(FStringView VirtualPath, ECS::FEntity DropTarget, bool bAttachToTarget, const FTransform* SpawnTransform = nullptr) {}

        // Attaches the script a .cs file declares to DropTarget, or to a new entity when there is none.
        virtual void HandleScriptSourceDrop(FStringView VirtualPath, ECS::FEntity DropTarget);

        virtual void OnDropSpawnedEntity(ECS::FEntity Entity) {}
        virtual bool IsComponentHiddenInDetails(const CStruct* Type) const;
        virtual void OnEntityCreatedInScene(ECS::FEntity Entity) {}
        virtual FTransform GetNewEntitySpawnTransform() const;
        virtual bool CanDeleteEntity(ECS::FEntity Entity) const { return true; }
        virtual void PersistGizmoSettings() {}
        virtual void PersistCameraPreviewScale() {}
        virtual bool AllowCameraPreview() const { return true; }
        virtual bool IsViewportPlaying() const { return false; }
        virtual void DrawViewportToolbarPlayControls(float ButtonSize) {}
        virtual void DrawViewportToolbarModeSelector(float ButtonSize) {}

        void OnAssetLoadFinished() final;
        void EndFrame() override;
        void DrawViewportToolbar(const FUpdateContext& UpdateContext) override;
        NODISCARD bool DrawsViewModeInViewportToolbar() const override { return true; }

        void MarkSceneDirty();

        // The world the outliner, details and selection inspect, which the world editor can point at another live world.
        NODISCARD CWorld* GetObservedWorld() const { return ObservedWorld != nullptr ? ObservedWorld : World.Get(); }
        NODISCARD bool IsInspectingForeignWorld() const { return ObservedWorld != nullptr && ObservedWorld != World.Get(); }
        NODISCARD ECS::FRegistry& GetSceneRegistry() const { return GetObservedWorld()->GetEntityRegistry(); }

        void SetSingleSelectedEntity(ECS::FEntity Entity);
        void AddSelectedEntity(ECS::FEntity Entity);
        void RemoveSelectedEntity(ECS::FEntity Entity);
        void ToggleSelectedEntity(ECS::FEntity Entity);
        void ClearSelectedEntities();

        // Null safe, since teardown runs after the world is gone.
        void ResetSelectionState();

        // Rebuilds the selection from the registry tags, after an undo or a world swap.
        void ResyncSelectionFromRegistry();

        // Restamps the unserialized selection tags from the CPU set, since a true restore keeps handles but drops tags.
        void ReapplySelectionTags();

        NODISCARD bool IsEntitySelected(ECS::FEntity Entity) const { return SelectedEntities.find(Entity) != SelectedEntities.end(); }
        NODISCARD const THashSet<ECS::FEntity>& GetSelectedEntities() const { return SelectedEntities; }
        NODISCARD ECS::FEntity GetLastSelectedEntity() const { return LastSelectedEntity; }

        // bInteractiveOnly drops the visualizers that have no main-thread pass.
        void GatherVisualizerBindings(TVector<FVisualizerBinding>& OutBindings, bool bInteractiveOnly);
        void GatherVisualizerEntities(TVector<ECS::FEntity>& OutEntities);

        // Grows the selection into what the wireframe pass draws, its children or with bShowHierarchyVisualizers every tree it is in.
        void ExpandVisualizerEntities(TVector<ECS::FEntity>& InOutEntities);

        void RebuildSceneOutliner(FTreeListView& Tree);
        FTreeNodeID AddEntityToOutliner(ECS::FEntity Entity);

        // Built on first hover, since the component scan behind a row's tooltip is expensive.
        void BuildEntityTooltip(ECS::FEntity Entity, FTreeNodeDisplay& Display);

        void RemoveEntityFromOutliner(ECS::FEntity Entity);
        void ReparentEntityInOutliner(ECS::FEntity Entity);
        void RefreshOutlinerExpander(ECS::FEntity Entity);
        void BuildEntityChildren(FTreeListView& Tree, FTreeNodeID Item);
        void FlushOutlinerPending();
        void FlushOutlinerRemovals();
        void SyncOutlinerRowSelection(ECS::FEntity Entity, bool bSelected);
        void DrawOutliner(bool bFocused);
        void DrawFilterOptions();
        NODISCARD size_t CountOutlinerEntities() const;
        void DrawAddToEntityOrWorldPopup(ECS::FEntity Entity = ECS::NullEntity);

        // The folder table lives on the world's singleton entity, which the outliner hides.
        NODISCARD const SSceneFolderComponent* GetSceneFolders() const;
        NODISCARD SSceneFolderComponent* GetEditableSceneFolders() const;
        NODISCARD ECS::FEntity GetSceneFolderOwner() const;
        NODISCARD uint32 GetEntityFolderID(ECS::FEntity Entity) const;
        NODISCARD FTreeNodeID FindFolderNode(uint32 FolderID) const;
        NODISCARD FString GetFolderPath(uint32 FolderID) const;
        NODISCARD FName MakeUniqueFolderName(uint32 ParentID) const;
        void BuildFolderNodes(FTreeListView& Tree);
        void CollectFolderEntities(uint32 FolderID, TVector<ECS::FEntity>& OutEntities, bool bRecursive) const;
        uint32 CreateSceneFolder(const FName& Name, uint32 ParentID);
        void RenameSceneFolder(uint32 FolderID, FStringView NewName);
        void DeleteSceneFolder(uint32 FolderID, bool bDeleteContents);
        void MoveFolderIntoFolder(uint32 FolderID, uint32 NewParentID);

        // Folder 0 unfiles the entities, and any parented to another entity are detached first.
        void MoveEntitiesToFolder(const TVector<ECS::FEntity>& Entities, uint32 FolderID);

        void SelectFolderContents(uint32 FolderID);
        void SetFolderContentsHidden(uint32 FolderID, bool bHidden);
        void DrawFolderContextMenu(uint32 FolderID);
        void DrawMoveToFolderMenuItems(const TVector<ECS::FEntity>& Entities);
        void DrawNewFolderButton(float ButtonSize);
        void BeginFolderTransaction();
        void RecordSceneFolderSnapshot();

        void AcceptContentBrowserPayload(ECS::FEntity DropTarget, bool bAttachToTarget);

        // The viewport as one drop target, moving a live preview of what a drag places; true while a drag hovers it.
        bool DrawViewportDropTarget();

        void HandleNewEntityDrop(const DragDrop::FPayload& Payload, ECS::FEntity DropTarget, bool bAttachToTarget, const FTransform* SpawnTransform = nullptr);
        void DecorateNewEntity(const DragDrop::FPayload& Payload, ECS::FEntity Entity);

        // An untransacted preview of what Payload places, replaced by a transacted drop on release.
        void SpawnPlacementPreview(const DragDrop::FPayload& Payload);
        void DestroyPlacementPreview();

        // Tab switches the wheel between turning and scaling the placement preview, Ctrl taking coarser steps.
        void HandlePlacementInput();

        void DrawPlacementAdjustment(const FViewportHit& Hit) const;
        NODISCARD FQuat GetPlacementRotation() const;

        void RebuildPropertyTables(ECS::FEntity Entity);

        // False once any table's component moved or went, as a swap remove from another entity does to it.
        NODISCARD bool AreDetailsTablesCurrent() const;

        void DrawDetailsPanel(bool bFocused);
        void DrawEntityProperties(ECS::FEntity Entity);
        void DrawEmptyState();
        void DrawComponentList(ECS::FEntity Entity);

        // One search box over the whole panel, matching a component by title and otherwise filtering into its table.
        void DrawComponentSearchBar();

        void DrawComponentHeader(FComponentTableEntry& Entry, ECS::FEntity Entity);
        void DrawStaticMeshCollisionHint(ECS::FEntity Entity, const SStaticMeshComponent& MeshComponent);
        void DrawAddCollisionMenuItem(ECS::FEntity Entity);
        void AddMeshCollisionTransacted(const TVector<ECS::FEntity>& Entities);

        void BuildSocketPickerData(ECS::FEntity Entity, FSocketEditContext& Out);

        void RemoveComponent(ECS::FEntity Entity, const CStruct* ComponentType);

        void ProcessComponentEditRequests();

        void OnPrePropertyChangeEvent(const FPropertyChangedEvent& Event);
        void OnPostPropertyChangeEvent(const FPropertyChangedEvent& Event);
        void BeginPropertyEditTransaction(CStruct* ComponentType);
        void RecordPrefabOverrideSnapshot(const TVector<ECS::FEntity>& Entities);
        void PushRenameEntityModal(ECS::FEntity Entity);
        void PushAddTagModal(ECS::FEntity Entity);
        void DrawTagSection(ECS::FEntity Entity);

        // Wraps the transaction, the creation hook, selection and dirty marking that every spawn shares.
        ECS::FEntity SpawnEntityTransacted(FName Name, FName TransactionLabel, const TFunction<void(ECS::FEntity)>& Decorate, const FTransform* SpawnTransform = nullptr);

        void CreateEntity();
        void CreateEntityWithComponent(const CStruct* Component);
        void CreatePrimitiveEntity(CStaticMesh* PrimitiveMesh, const char* DisplayName);

        // The whole selection when Entity is part of a multi-selection, otherwise just Entity.
        TVector<ECS::FEntity> GetComponentEditTargets(ECS::FEntity Entity);

        void ApplyAddComponentToTargets(const TVector<ECS::FEntity>& Targets, CStruct* PickedMetaType);

        // True on a click; with OutDragging set, rows also drag out as new entities and it reports whether one is.
        bool DrawAddableComponentList(const ImGuiTextFilter& Filter, const TVector<ECS::FEntity>& Targets, CStruct*& OutMetaType, CStruct*& OutStruct, bool* OutDragging = nullptr);

        // The entity clipboard, mirrored on the registry as FCopiedTag.
        void AddEntityToCopies(ECS::FEntity Entity);
        void RemoveEntityFromCopies(ECS::FEntity Entity);
        void ClearCopies() const;
        void CopyEntity(ECS::FEntity& To, ECS::FEntity From);

        void CycleGuizmoOp();
        void ToggleGuizmoMode();
        void DrawCameraControls(float ButtonSize);
        void DrawViewportOptions(float ButtonSize);
        void DrawSnapSettingsPopup();
        void UpdateCameraPreview();
        void DrawCameraPreviewOverlay(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize);

        static constexpr uint32                 CameraPreviewWidth = 720;
        static constexpr uint32                 CameraPreviewHeight = 405;

        CWorld*                                 ObservedWorld = nullptr;
        THashSet<ECS::FEntity>                  SelectedEntities;
        ECS::FEntity                            LastSelectedEntity = ECS::NullEntity;

        THashMap<uint32, CComponentVisualizer*> VisualizerCache;
        SIZE_T                                  CachedVisualizerCount = ~(SIZE_T)0;
        TVector<ECS::FEntity>                   VisualizerEntityScratch;
        TVector<FVisualizerBinding>             VisualizerBindingScratch;
        FVisualizerInteractionState             VisualizerInteraction;

        FTreeListView                           OutlinerListView;
        FTreeListViewContext                    OutlinerContext;
        FEntityListFilterState                  EntityFilterState;
        ImGuiTextFilter                         AddEntityComponentFilter;
        THashMap<ECS::FEntity, FTreeNodeID>     EntityToTreeNode;
        THashMap<uint32, FTreeNodeID>           FolderToTreeNode;
        THashMap<ECS::FEntity, uint32>          EntityFolderCache;
        TVector<ECS::FEntity>                   PendingOutlinerAdds;
        TVector<ECS::FEntity>                   PendingExpanderRefresh;

        // Removed in one batch at the next flush, since a streaming burst destroys thousands.
        TVector<ECS::FEntity>                   PendingOutlinerRemovals;
        TVector<ECS::FEntity>                   PendingFolderRemovals;

        // A freshly created folder, whose row enters inline rename on the next draw.
        uint32                                  PendingFolderRename = 0;
        bool                                    bOutlinerActive = false;

        ECS::FEntity                            PlacementPreview = ECS::NullEntity;
        CObject*                                PlacementPreviewAsset = nullptr;
        TVector<ECS::FEntity>                   PlacementPreviewEntities;
        float                                   PlacementYawDegrees = 0.0f;
        float                                   PlacementScale = 1.0f;

        // Smooth trackpads report fractions of a notch, kept here until they add up to one.
        float                                   PlacementWheelCarry = 0.0f;
        bool                                    bPlacementScaling = false;
        bool                                    bPlacementPreviewAttempted = false;

        // The renderer's copy of the scene from before the preview existed, which the placement reads under it.
        bool                                    bPlacementSnapshotRequested = false;

        TVector<FComponentTableEntry>           PropertyTables;
        TVector<ECS::FEntity>                   DetailsOtherEntities;
        ImGuiTextFilter                         DetailsFilter;
        ECS::FEntity                            DetailsEntity = ECS::NullEntity;
        TQueue<FComponentDestroyRequest>        ComponentDestroyRequests;
        TQueue<ECS::FEntity>                    EntityDestroyRequests;
        bool                                    bDetailsDirty = false;

        // A table points into a value buffer and a layout that a script reload frees, so the next draw drops them all.
        bool                                    bDetailsScriptsReloaded = false;

        // A prefab refresh rewrites an instance's components and replaces the baselines its reset-to-prefab points into.
        uint32                                  DetailsPrefabGeneration = 0;

        // Armed by a selection change and consumed by the next tick, rather than resolving transforms every frame.
        bool                                    bSelectionTransformRefreshPending = false;

        FPropertyEditContext                    PropertyContext;
        FAnimGraphEditContext                   AnimGraphCtx;
        FSocketEditContext                      SocketCtx;
        FWorldEditContext                       WorldCtx;
        FEntityPickContext                      PickCtx;

        // Owned by the tool rather than the modal lambda, which cannot hold state across frames.
        FFixedString                            TextModalBuffer;

        ImGuizmo::OPERATION                     GuizmoOp = ImGuizmo::TRANSLATE;
        ImGuizmo::MODE                          GuizmoMode = ImGuizmo::WORLD;
        float                                   GuizmoSnapTranslate = 0.1f;
        float                                   GuizmoSnapRotate = 5.0f;
        float                                   GuizmoSnapScale = 0.1f;
        bool                                    bGuizmoSnapEnabled = true;
        bool                                    bImGuizmoUsedOnce = false;

        IRenderScene*                           CameraPreviewScene = nullptr;
        int32                                   CameraPreviewHandle = -1;
        float                                   CameraPreviewScale = 0.6f;
        bool                                    bCameraPreviewActive = false;
        bool                                    bCameraPreviewResizing = false;

        // Last frame's grip hover or drag, which keeps a resize from falling through to viewport picking.
        bool                                    bCameraPreviewMouseOver = false;

        // Authored rather than derived each frame, since re-deriving a rotation snaps 179 to a gimbal-equivalent triple.
        FVector3                                CameraEulerDegrees = FVector3(0.0f);
        FQuat                                   CameraEulerRotationSource = FQuat::Identity();
    };
}
