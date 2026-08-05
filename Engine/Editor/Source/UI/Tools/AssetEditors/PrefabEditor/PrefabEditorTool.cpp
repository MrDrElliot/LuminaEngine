#include "PrefabEditorTool.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Components/EditorEntityTags.h"
#include "Config/Config.h"
#include "Core/Object/ObjectCore.h"
#include "Settings/EditorSettings.h"
#include "Core/Math/Math.h"
#include "UI/Tools/ContentBrowserEditorTool.h"
#include "UI/Tools/EditorEntityUtils.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "EASTL/sort.h"
#include "GUID/GUID.h"
#include "Core/Math/Math.h"
#include "Tools/ComponentVisualizers/ComponentVisualizer.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyTable.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Traits.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/World.h"


namespace Lumina
{
    FPrefabEditorTool::FPrefabEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FSceneEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    CPrefab* FPrefabEditorTool::GetPrefab() const
    {
        return Cast<CPrefab>(Asset.Get());
    }

    void FPrefabEditorTool::OnInitialize()
    {
        CreateToolWindow(OutlinerWindowName, [this](bool bFocused)
        {
            DrawOutliner(bFocused);
        });

        CreateToolWindow(PropertiesWindowName, [this](bool bFocused)
        {
            DrawDetailsPanel(bFocused);
        });

        const CPrefabEditorSettings* Settings = GetDefault<CPrefabEditorSettings>();
        bGuizmoSnapEnabled  = Settings->bGizmoSnapEnabled;
        GuizmoSnapTranslate = Settings->GizmoSnapTranslate;
        GuizmoSnapRotate    = Settings->GizmoSnapRotate;
        GuizmoSnapScale     = Settings->GizmoSnapScale;
        CameraPreviewScale  = Math::Clamp(Settings->CameraPreviewScale, 0.25f, 1.5f);

        OutlinerContext.RebuildTreeFunction = [this](FTreeListView& Tree)
        {
            RebuildSceneOutliner(Tree);
        };

        OutlinerContext.BuildChildrenFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            BuildEntityChildren(Tree, Item);
        };

        OutlinerContext.FilterFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            const FTreeNodeDisplay& Display = Tree.Get<FTreeNodeDisplay>(Item);
            return EntityFilterState.FilterName.PassFilter(Display.DisplayName.c_str());
        };

        OutlinerContext.ItemSelectedFunction = [this](FTreeListView& Tree, FTreeNodeID Item, bool bShouldClear)
        {
            // Plain click replaces; Ctrl-click toggles. Mirrors WorldEditor.
            if (!Item.IsValid())
            {
                if (bShouldClear)
                {
                    ClearSelectedEntities();
                }
                return;
            }

            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            if (Data.Entity == entt::null || !World->IsValidEntity(Data.Entity))
            {
                return;
            }

            if (bShouldClear)
            {
                SetSingleSelectedEntity(Data.Entity);
            }
            else
            {
                ToggleSelectedEntity(Data.Entity);
            }
        };

        OutlinerContext.ItemDoubleClickedFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            FocusViewportToEntity(Data.Entity);
        };

        OutlinerContext.ItemContextMenuFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            entt::registry& Registry = ECS::GetWorldRegistry(*World);

            if (!Registry.valid(Data.Entity))
            {
                return;
            }

            const entt::entity Root = FindPrefabRoot();
            const bool bIsRoot = (Data.Entity == Root);

            if (ImGui::MenuItem("Copy Entity ID"))
            {
                ImGui::SetClipboardText(eastl::to_string(entt::to_integral(Data.Entity)).c_str());
            }

            if (!bIsRoot && ECS::Utils::IsChild(Registry, Data.Entity))
            {
                if (ImGui::MenuItem("Unparent"))
                {
                    BeginTransaction();
                    ECS::Utils::RemoveFromParent(Registry, Data.Entity);
                    EndTransaction("Unparent");
                    OutlinerListView.MarkTreeDirty();
                }
            }

            if (ECS::Utils::IsParent(Registry, Data.Entity))
            {
                if (ImGui::MenuItem("Detach Children"))
                {
                    TVector<entt::entity> Children;
                    ECS::Utils::ForEachChild(Registry, Data.Entity, [&](entt::entity Child) { Children.push_back(Child); });
                    BeginTransaction();
                    ECS::Utils::DetachImmediateChildren(Registry, Data.Entity);
                    EndTransaction("Detach Children");
                    OutlinerListView.MarkTreeDirty();
                }
            }

            if (!bIsRoot && ImGui::MenuItem(LE_ICON_CONTENT_DUPLICATE " Duplicate"))
            {
                BeginTransaction();
                entt::entity New = DuplicatePrefabEntity(Data.Entity);
                if (New != entt::null)
                {
                    EndTransaction("Duplicate");
                    OutlinerListView.MarkTreeDirty();
                }
                else
                {
                    AbortTransaction();
                }
            }

            if (!bIsRoot && ImGui::MenuItem(LE_ICON_DELETE " Delete"))
            {
                RequestDestroyEntity(Data.Entity);
            }
        };

        OutlinerContext.SetDragDropFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            DragDrop::SetEntityPayload(World, Data.Entity);
        };

        OutlinerContext.DragDropFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            HandleOutlinerDragDrop(Tree, Data.Entity);
        };

        OutlinerContext.RenameFunction = [this](FTreeListView& Tree, FTreeNodeID Item, FStringView NewName)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            entt::registry& Registry = ECS::GetWorldRegistry(*World);
            if (!Registry.valid(Data.Entity))
            {
                return;
            }

            BeginTransaction();
            if (SNameComponent* NameComp = Registry.try_get<SNameComponent>(Data.Entity))
            {
                NameComp->Name = NewName;
            }
            EndTransaction("Rename");

            const SNameComponent* NameComp = Registry.try_get<SNameComponent>(Data.Entity);
            Tree.Get<FTreeNodeDisplay>(Item).DisplayName = EditorEntityUtils::MakeOutlinerDisplayName(NameComp, Data.Entity).c_str();
            Asset->GetPackage()->MarkDirty();
        };

        OutlinerContext.VisibilityToggleFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            FTreeNodeState& State = Tree.Get<FTreeNodeState>(Item);
            if (!World->IsValidEntity(Data.Entity))
            {
                return;
            }
            if (State.bDisabled)
            {
                World->EmplaceComponent<SDisabledTag>(Data.Entity);
            }
            else
            {
                World->RemoveComponent<SDisabledTag>(Data.Entity);
            }
        };

        OutlinerContext.HoveredFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            entt::registry& Registry = ECS::GetWorldRegistry(*World);
            if (!Registry.valid(Data.Entity))
            {
                return;
            }

            // Tooltip building (component scan) is deferred from tree-build to first hover.
            BuildEntityTooltip(Data.Entity, Tree.Get<FTreeNodeDisplay>(Item));

            if (const STransformComponent* Transform = Registry.try_get<STransformComponent>(Data.Entity))
            {
                if (const SStaticMeshComponent* MeshComp = Registry.try_get<SStaticMeshComponent>(Data.Entity))
                {
                    World->DrawBox(Transform->GetWorldLocation(), MeshComp->GetAABB().GetSize() * 0.5f * Transform->GetWorldScale() * 1.2f, Transform->GetWorldRotation(), FColor::White, 3.0f);
                }
                else
                {
                    World->DrawBox(Transform->GetWorldLocation(), 1.0f * Transform->GetWorldScale(), Transform->GetWorldRotation(), FColor::White, 3.0f);
                }
            }
        };

        OutlinerContext.KeyPressedFunction = [this](FTreeListView& Tree, FTreeNodeID Item, ImGuiKey Key) -> bool
        {
            if (Key == ImGuiKey_Delete)
            {
                FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
                RequestDestroyEntity(Data.Entity);
                return true;
            }
            return false;
        };

        RegisterEditorActions();
    }

    void FPrefabEditorTool::RegisterEditorActions()
    {
        // Hotkeys are suppressed while right-mouse flying so W/E/R/Q act as fly keys, not mode switches.
        auto Hovered = [this]() { return bViewportHovered && !CameraState.bWasLooking; };
        auto AlwaysOn = []() { return true; };

        RegisterAction({"Translate Mode", "Gizmo", "Switch the gizmo to translate (move) mode",
            FInputChord{ImGuiKey_W}, [this]{ GuizmoOp = ImGuizmo::TRANSLATE; }, Hovered});

        RegisterAction({"Rotate Mode", "Gizmo", "Switch the gizmo to rotate mode",
            FInputChord{ImGuiKey_E}, [this]{ GuizmoOp = ImGuizmo::ROTATE; }, Hovered});

        RegisterAction({"Scale Mode", "Gizmo", "Switch the gizmo to scale mode",
            FInputChord{ImGuiKey_R}, [this]{ GuizmoOp = ImGuizmo::SCALE; }, Hovered});

        RegisterAction({"Toggle Local/World", "Gizmo", "Switch the gizmo between world-space and entity-local space",
            FInputChord{ImGuiKey_X}, [this]{ EditorEntityUtils::ToggleGizmoMode(GuizmoMode); }, Hovered});

        RegisterAction({"Focus Selection", "View", "Frame the camera on the last-selected entity",
            FInputChord{ImGuiKey_F}, [this]{ FocusViewportToEntity(GetLastSelectedEntity()); }});

        RegisterAction({"Frame All", "View", "Frame the camera on every prefab entity",
            FInputChord{ImGuiKey_Home}, [this]{ FrameAllEntities(); }, Hovered});

        RegisterAction({"Reset Transform", "Selection", "Reset the selected entities' local transform to identity",
            FInputChord{ImGuiKey_R, true, true}, [this]{ ResetSelectionTransform(); }, Hovered});

        RegisterAction({"Undo", "History", "Revert the last transacted edit",
            FInputChord{ImGuiKey_Z, true}, [this]{ Undo(); }, AlwaysOn});

        RegisterAction({"Redo", "History", "Re-apply the last undone edit",
            FInputChord{ImGuiKey_Y, true}, [this]{ Redo(); }, AlwaysOn});

        // Advisory entries: inline-handled shortcuts surfaced in Help > Keybinds.
        RegisterAction({"Duplicate", "Selection", "Duplicate the selection in place",
            FInputChord{ImGuiKey_D, true}, nullptr});
        RegisterAction({"Copy", "Selection", "Copy the selection to the entity clipboard",
            FInputChord{ImGuiKey_C, true}, nullptr});
        RegisterAction({"Paste", "Selection", "Paste previously-copied entities under the prefab root",
            FInputChord{ImGuiKey_V, true}, nullptr});
        RegisterAction({"Delete", "Selection", "Delete the selected entities",
            FInputChord{ImGuiKey_Delete}, nullptr});
        RegisterAction({"Cycle Gizmo", "Gizmo", "Cycle Translate→Rotate→Scale",
            FInputChord{ImGuiKey_Space}, nullptr});
    }

    void FPrefabEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();

        DirectionalLightEntity = World->ConstructEntity("PreviewLight");
        World->EmplaceComponent<FHideInSceneOutliner>(DirectionalLightEntity);
        World->EmplaceComponent<SDirectionalLightComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SSkyLightComponent>(DirectionalLightEntity);
    }

    void FPrefabEditorTool::OnSceneLoaded()
    {
        LoadPrefabIntoPreviewWorld();
        OutlinerListView.MarkTreeDirty();

        // Loading the prefab repopulates the registry; nothing before this point is meaningful
        // to undo back into.
        ClearTransactionHistory();

        // Frame the freshly loaded prefab so the preview camera isn't dropped on origin
        // when the prefab is offset away from world zero.
        FrameAllEntities();
    }

    void FPrefabEditorTool::OnPostUndoRedo()
    {
        // The true-restore preserves handles but drops the (non-serialized) selection tags; re-stamp then rebuild the cache.
        ReapplySelectionTags();
        ResyncSelectionFromRegistry();

        // Component pointers in PropertyTables are stale after the registry serialize.
        OutlinerListView.MarkTreeDirty();
        bDetailsDirty = true;
        DetailsEntity = entt::null;
    }

    void FPrefabEditorTool::LoadPrefabIntoPreviewWorld()
    {
        CPrefab* Prefab = GetPrefab();
        if (Prefab == nullptr || World == nullptr)
        {
            return;
        }

        // Wipe any previously-loaded prefab entities (leave preview-only lights / floor / camera).
        entt::registry& WorldRegistry = ECS::GetWorldRegistry(*World);
        TVector<entt::entity> ToDestroy;
        WorldRegistry.view<SPrefabComponent>().each([&](entt::entity E, const SPrefabComponent&)
        {
            ToDestroy.push_back(E);
        });
        for (entt::entity E : ToDestroy)
        {
            if (WorldRegistry.valid(E))
            {
                ECS::Utils::DestroyEntityHierarchy(WorldRegistry, E);
            }
        }

        ClearSelectedEntities();

        // Empty prefab: seed a single root the user can edit.
        if (Prefab->Registry.view<entt::entity>().empty())
        {
            entt::entity Root = WorldRegistry.create();
            WorldRegistry.emplace<SNameComponent>(Root).Name = FName("Root");
            WorldRegistry.emplace<STransformComponent>(Root);
            WorldRegistry.emplace<SPrefabComponent>(Root).StableID = FName(FGuid::New().ToShortString());
            SetSingleSelectedEntity(Root);
            return;
        }

        THashMap<entt::entity, entt::entity> Map;
        CPrefab::CopyRegistry(Prefab->Registry, WorldRegistry, Map);

        // Auto-select the root so the property panel isn't empty on first load.
        const entt::entity Root = FindPrefabRoot();
        if (Root != entt::null)
        {
            SetSingleSelectedEntity(Root);
        }
    }

    void FPrefabEditorTool::CommitPreviewWorldToPrefab()
    {
        CPrefab* Prefab = GetPrefab();
        if (Prefab == nullptr || World == nullptr)
        {
            return;
        }

        entt::registry& WorldRegistry = ECS::GetWorldRegistry(*World);

        // Gather every entity that belongs to the prefab (tagged with SPrefabComponent); the
        // preview world also holds preview-only lights/floor/camera that must not be captured.
        TVector<entt::entity> PrefabEntities;
        WorldRegistry.view<SPrefabComponent>().each([&](entt::entity E, const SPrefabComponent&)
        {
            PrefabEntities.push_back(E);
        });

        // Rebuild from scratch (avoids accumulating dead entities). CopyRegistry copies just the
        // tagged subset, skips the editor-only set, and remaps hierarchy + entity-handle fields.
        // Replacing the registry frees the components any open details panel points at for its
        // reset-to-prefab baseline, so the generation has to move with it.
        CPrefab::BumpDataGeneration();
        Prefab->Registry = entt::registry{};
        THashMap<entt::entity, entt::entity> SrcToDst;
        CPrefab::CopyRegistry(WorldRegistry, Prefab->Registry, SrcToDst, &PrefabEntities,
            +[](entt::id_type ID) { return EditorEntityUtils::IsEditorOnlyComponent(ID); });
    }

    void FPrefabEditorTool::CommitScene()
    {
        CommitPreviewWorldToPrefab();
    }

    void FPrefabEditorTool::OnSave()
    {
        // Persist gizmo prefs alongside the asset save so they survive across editor sessions.
        PersistGizmoSettings();

        // Super commits the preview world into the prefab (CommitScene) then saves the package.
        Super::OnSave();

        // Push the just-committed edits onto every live instance in open worlds so they update
        // immediately instead of only on the next world load.
        if (CPrefab* Prefab = GetPrefab())
        {
            Prefab->RefreshInstancesInLoadedWorlds();
        }
    }

    void FPrefabEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        // The preview world never draws editor billboards (light/camera icons). Re-applied every
        // frame because the render scene (and its settings) can be rebuilt by idle reclaim.
        if (IRenderScene* Renderer = World->GetRenderer())
        {
            Renderer->GetSceneRenderSettings().bDrawBillboards = false;
        }

        // Drive the selected-camera preview (shared with the world editor) before extract.
        UpdateCameraPreview();

        ProcessDestroyRequests();

        // Mark selection's transform dirty so the gizmo's edits propagate to children this frame.
        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        Registry.view<FSelectedInEditorComponent>().each([&](entt::entity Entity)
        {
            Registry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
        });

        const entt::entity LastSelected = GetLastSelectedEntity();
        if (Registry.valid(LastSelected))
        {
            // RebuildPropertyTables sets DetailsEntity + clears bDetailsDirty internally.
            if (LastSelected != DetailsEntity || bDetailsDirty)
            {
                RebuildPropertyTables(LastSelected);
            }
        }
        else if (!PropertyTables.empty() || DetailsEntity != entt::null)
        {
            PropertyTables.clear();
            DetailsEntity = entt::null;
            bDetailsDirty = false;
        }

        // Drain queued reflected-component removals (the shared DrawComponentHeader pushes here).
        ProcessComponentEditRequests();

        if (bViewportHovered)
        {
            // Delete every selected entity (root is filtered out inside the destroy queue).
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))
            {
                Registry.view<FSelectedInEditorComponent>().each([&](entt::entity Entity)
                {
                    RequestDestroyEntity(Entity);
                });
            }
        }

        ProcessClipboardShortcuts();

        // Selection-highlight bounds; mirror world editor's red-AABB.
        Registry.view<FSelectedInEditorComponent>().each([&](entt::entity Entity)
        {
            if (!Registry.valid(Entity))
            {
                return;
            }
            if (SStaticMeshComponent* MeshComp = Registry.try_get<SStaticMeshComponent>(Entity))
            {
                const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);
                World->DrawBox(Transform.GetWorldLocation(), MeshComp->GetAABB().GetSize() * 0.5f * Transform.GetWorldScale() * 1.2f, Transform.GetWorldRotation(), FColor::Red, 5.0f);
            }
        });
    }

    void FPrefabEditorTool::ProcessDestroyRequests()
    {
        if (EntityDestroyRequests.empty())
        {
            return;
        }

        BeginTransaction();
        bool bDestroyed = false;
        const entt::entity Root = FindPrefabRoot();

        while (!EntityDestroyRequests.empty())
        {
            entt::entity Entity = EntityDestroyRequests.back();
            EntityDestroyRequests.pop();

            if (Entity == entt::null || !World->IsValidEntity(Entity))
            {
                continue;
            }

            // Don't allow destroying the prefab root; a prefab must have one.
            if (Entity == Root)
            {
                ImGuiX::Notifications::NotifyError("Cannot delete the prefab root entity.");
                continue;
            }

            // Drop selection links so the selection set doesn't carry stale entities into next frame.
            RemoveSelectedEntity(Entity);
            ECS::Utils::ForEachDescendant(ECS::GetWorldRegistry(*World), Entity, [&](entt::entity Desc)
            {
                RemoveSelectedEntity(Desc);
            });

            ECS::Utils::DestroyEntityHierarchy(ECS::GetWorldRegistry(*World), Entity);
            bDestroyed = true;
            OutlinerListView.MarkTreeDirty();
            Asset->GetPackage()->MarkDirty();
        }

        if (bDestroyed)
        {
            EndTransaction("Delete Entity");
        }
        else
        {
            AbortTransaction();
        }
    }

    void FPrefabEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        PropertyTables.clear();
        SelectedEntities.clear();
    }

    entt::entity FPrefabEditorTool::FindPrefabRoot() const
    {
        if (World == nullptr)
        {
            return entt::null;
        }

        entt::registry& WorldRegistry = ECS::GetWorldRegistry(*World);
        entt::entity Root = entt::null;
        WorldRegistry.view<SPrefabComponent>().each([&](entt::entity E, const SPrefabComponent&)
        {
            if (Root != entt::null) return;

            const FRelationshipComponent* Rel = WorldRegistry.try_get<FRelationshipComponent>(E);
            const bool bHasPrefabParent = Rel && Rel->Parent != entt::null &&
                WorldRegistry.any_of<SPrefabComponent>(Rel->Parent);
            if (!bHasPrefabParent)
            {
                Root = E;
            }
        });
        return Root;
    }

    void FPrefabEditorTool::OnEntityCreatedInScene(entt::entity Entity)
    {
        entt::registry& Registry = ECS::GetWorldRegistry(*World);

        // Resolve the root BEFORE tagging the new entity, or it becomes a root candidate itself.
        const entt::entity Root = FindPrefabRoot();

        Registry.emplace<SPrefabComponent>(Entity).StableID = FName(FGuid::New().ToShortString());

        if (Root != entt::null && Root != Entity)
        {
            ECS::Utils::ReparentEntity(Registry, Entity, Root);
        }

        OutlinerListView.MarkTreeDirty();
    }

    FTransform FPrefabEditorTool::GetNewEntitySpawnTransform() const
    {
        // New prefab entities parent under the root at identity, not at the editor camera.
        return FTransform();
    }

    void FPrefabEditorTool::RequestDestroyEntity(entt::entity Entity)
    {
        if (Entity == entt::null)
        {
            return;
        }
        EntityDestroyRequests.push(Entity);
    }

    bool FPrefabEditorTool::IsComponentHiddenInDetails(const CStruct* Type) const
    {
        // Hide tags (base) plus the prefab's internal bookkeeping component.
        return Super::IsComponentHiddenInDetails(Type) || (Type != nullptr && Type->GetName() == FName("SPrefabComponent"));
    }

    void FPrefabEditorTool::ResetSelectionTransform()
    {
        if (World == nullptr)
        {
            return;
        }

        entt::registry& Registry = ECS::GetWorldRegistry(*World);

        TFixedVector<entt::entity, 64> Targets;
        Registry.view<FSelectedInEditorComponent>().each([&](entt::entity Selected)
        {
            if (Registry.valid(Selected))
            {
                Targets.push_back(Selected);
            }
        });

        if (Targets.empty())
        {
            return;
        }

        BeginTransaction();
        for (entt::entity Entity : Targets)
        {
            if (STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity))
            {
                Transform->SetLocalLocation(FVector3(0.0f));
                Transform->SetLocalRotation(FQuat(1.0f, 0.0f, 0.0f, 0.0f));
                Transform->SetLocalScale(FVector3(1.0f));
                Registry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
            }
        }
        EndTransaction("Reset Transform");
        Asset->GetPackage()->MarkDirty();
    }

    void FPrefabEditorTool::FrameAllEntities()
    {
        if (World == nullptr)
        {
            return;
        }

        const entt::entity Root = FindPrefabRoot();
        if (Root == entt::null)
        {
            return;
        }

        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        FVector3 Center;
        float Radius;
        if (!EditorEntityUtils::ComputeFocusBoundsForEntity(Registry, Root, Center, Radius))
        {
            return;
        }

        if (!Registry.valid(EditorEntity))
        {
            return;
        }

        const SCameraComponent& Camera = Registry.get<SCameraComponent>(EditorEntity);
        const float HalfFov  = Math::Radians(Camera.GetFOV() * 0.5f);
        const float Distance = (Radius / Math::Tan(Math::Max(HalfFov, Math::Radians(1.0f)))) * 1.5f;

        STransformComponent& EditorTransform = Registry.get<STransformComponent>(EditorEntity);
        const FVector3 Forward = EditorTransform.GetForward();
        EditorTransform.SetLocation(Center - Forward * Distance);
        EditorTransform.SetRotation(Math::FindLookAtRotation(Center, Center - Forward * Distance));
    }

    bool FPrefabEditorTool::IsOutlinerEntityVisible(entt::entity Entity) const
    {
        // Only prefab-owned entities belong in the outliner; preview lights/floor/camera are hidden.
        return Super::IsOutlinerEntityVisible(Entity) && GetSceneRegistry().any_of<SPrefabComponent>(Entity);
    }

    void FPrefabEditorTool::HandleOutlinerDragDrop(FTreeListView& Tree, entt::entity DropItem)
    {
        const DragDrop::FPayload* Peek = DragDrop::PeekPayload();
        if (Peek == nullptr)
        {
            return;
        }

        if (Peek->Kind == DragDrop::EPayloadKind::Entity)
        {
            CWorld* SourceWorld = nullptr;
            entt::entity Source = entt::null;
            if (DragDrop::AcceptEntity(&SourceWorld, &Source) && SourceWorld == World)
            {
                if (Source == entt::null || Source == DropItem)
                {
                    return;
                }

                entt::registry& Registry = ECS::GetWorldRegistry(*World);
                if (!Registry.valid(Source) || (DropItem != entt::null && !Registry.valid(DropItem)))
                {
                    return;
                }

                // Don't reparent the prefab root; hierarchy stays single-rooted.
                if (Source == FindPrefabRoot() || DropItem == entt::null)
                {
                    return;
                }

                // Cycle guard: dropping a parent into one of its descendants would form a loop.
                if (ECS::Utils::IsDescendantOf(Registry, DropItem, Source))
                {
                    ImGuiX::Notifications::NotifyError("Cannot reparent: target is a descendant of the dragged entity.");
                    return;
                }

                BeginTransaction();
                ECS::Utils::ReparentEntity(Registry, Source, DropItem);
                EndTransaction("Reparent");
                OutlinerListView.MarkTreeDirty();
                Asset->GetPackage()->MarkDirty();
            }
            return;
        }

        // Asset drop (static mesh, material, etc.) onto an outliner row.
        if (Peek->Kind == DragDrop::EPayloadKind::Asset && DragDrop::IsDelivered())
        {
            HandlePrefabContentDrop(FStringView(Peek->AssetPath.c_str(), Peek->AssetPath.size()), DropItem, /*bAttachToTarget*/ true);
        }
    }

    void FPrefabEditorTool::HandlePrefabContentDrop(FStringView VirtualPath, entt::entity DropTarget, bool bAttachToTarget)
    {
        // Default drop target is the prefab root so dropped meshes become prefab-owned children.
        if (DropTarget == entt::null)
        {
            DropTarget = FindPrefabRoot();
        }

        BeginTransaction();
        entt::entity Spawned = HandleContentBrowserAssetDrop(VirtualPath, DropTarget, bAttachToTarget);
        if (Spawned != entt::null && Spawned != DropTarget)
        {
            // Mark the freshly created entity as part of the prefab so it round-trips on save.
            entt::registry& Registry = ECS::GetWorldRegistry(*World);
            if (!Registry.any_of<SPrefabComponent>(Spawned))
            {
                Registry.emplace<SPrefabComponent>(Spawned).StableID = FName(FGuid::New().ToShortString());
            }
            EndTransaction("Drop Asset");
            OutlinerListView.MarkTreeDirty();
            Asset->GetPackage()->MarkDirty();
        }
        else if (Spawned == DropTarget && Spawned != entt::null)
        {
            // Existing entity was modified in-place (e.g. material override applied to mesh slot).
            EndTransaction("Drop Asset");
            Asset->GetPackage()->MarkDirty();
            bDetailsDirty = true;
        }
        else
        {
            AbortTransaction();
        }
    }

    entt::entity FPrefabEditorTool::DuplicatePrefabEntity(entt::entity Source)
    {
        if (Source == entt::null || World == nullptr)
        {
            return entt::null;
        }

        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        if (!Registry.valid(Source))
        {
            return entt::null;
        }

        entt::entity NewEntity = entt::null;
        World->DuplicateEntity(NewEntity, Source, &EditorEntityUtils::DefaultDuplicateFilter);

        if (NewEntity == entt::null)
        {
            return entt::null;
        }

        // Force unique stable IDs across the subtree; the duplicate copies the source's
        // SPrefabComponent verbatim, which would collide with its entity-pairing on save.
        auto FreshenStableID = [&](entt::entity E)
        {
            if (SPrefabComponent* Prefab = Registry.try_get<SPrefabComponent>(E))
            {
                Prefab->StableID = FName(FGuid::New().ToShortString());
            }
        };
        FreshenStableID(NewEntity);
        ECS::Utils::ForEachDescendant(Registry, NewEntity, FreshenStableID);

        // Re-parent under the source's parent (or the prefab root if it had none).
        const FRelationshipComponent* SourceRel = Registry.try_get<FRelationshipComponent>(Source);
        entt::entity Parent = (SourceRel && SourceRel->Parent != entt::null) ? SourceRel->Parent : FindPrefabRoot();
        if (Parent != entt::null && Parent != NewEntity)
        {
            ECS::Utils::ReparentEntity(Registry, NewEntity, Parent);
        }

        return NewEntity;
    }

    void FPrefabEditorTool::ProcessClipboardShortcuts()
    {
        if (!bViewportHovered || World == nullptr)
        {
            return;
        }

        const ImGuiIO& IO = ImGui::GetIO();
        const bool bCopyPressed      = IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C);
        const bool bDuplicatePressed = IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D);
        const bool bPastePressed     = IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false);

        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        const entt::entity Root = FindPrefabRoot();

        // Ctrl+S is deliberately absent: FEditorUI routes it to the focused tool's OnSave. Handling it
        // here as well saved twice when focused, and saved the prefab merely because the cursor was
        // over its viewport while a different tool had focus.

        if (bCopyPressed)
        {
            Registry.clear<FCopiedTag>();
            Registry.view<FSelectedInEditorComponent>().each([&](entt::entity Selected)
            {
                if (Selected != Root)
                {
                    Registry.emplace_or_replace<FCopiedTag>(Selected);
                }
            });
        }

        auto DuplicateBatch = [&](TFixedVector<entt::entity, 64>& Sources, FName Label)
        {
            if (Sources.empty())
            {
                return;
            }

            BeginTransaction();
            TFixedVector<entt::entity, 64> NewlyCreated;
            for (entt::entity Source : Sources)
            {
                entt::entity New = DuplicatePrefabEntity(Source);
                if (New != entt::null)
                {
                    NewlyCreated.push_back(New);
                }
            }

            if (!NewlyCreated.empty())
            {
                ClearSelectedEntities();
                for (entt::entity New : NewlyCreated)
                {
                    AddSelectedEntity(New, false);
                }
                EndTransaction(Label);
                OutlinerListView.MarkTreeDirty();
                Asset->GetPackage()->MarkDirty();
            }
            else
            {
                AbortTransaction();
            }
        };

        if (bDuplicatePressed)
        {
            TFixedVector<entt::entity, 64> Sources;
            Registry.view<FSelectedInEditorComponent>().each([&](entt::entity Selected)
            {
                if (Selected != Root)
                {
                    Sources.push_back(Selected);
                }
            });
            DuplicateBatch(Sources, "Duplicate");
        }

        if (bPastePressed)
        {
            TFixedVector<entt::entity, 64> Sources;
            Registry.view<FCopiedTag>().each([&](entt::entity Tagged)
            {
                if (Tagged != Root)
                {
                    Sources.push_back(Tagged);
                }
            });
            DuplicateBatch(Sources, "Paste");
        }
    }

    void FPrefabEditorTool::HandleOutlinerEmptyAreaDrop()
    {
        // A content-browser asset dropped on empty space spawns under the prefab root.
        const DragDrop::FPayload* Peek = DragDrop::PeekPayload();
        if (Peek && Peek->Kind == DragDrop::EPayloadKind::Asset && DragDrop::IsDelivered())
        {
            HandlePrefabContentDrop(FStringView(Peek->AssetPath.c_str(), Peek->AssetPath.size()), entt::null, /*bAttachToTarget*/ false);
        }
    }

    bool FPrefabEditorTool::CanDeleteEntity(entt::entity Entity) const
    {
        // The prefab root is structural and cannot be deleted.
        return Entity != FindPrefabRoot();
    }

    void FPrefabEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        // Cycle gizmo op on Space, like the world editor.
        if (bViewportHovered && ImGui::IsKeyPressed(ImGuiKey_Space))
        {
            EditorEntityUtils::CycleGizmoOp(GuizmoOp);
        }

        SCameraComponent* CameraComponent = World->TryGetComponent<SCameraComponent>(EditorEntity);
        if (CameraComponent == nullptr)
        {
            return;
        }

        FMatrix4 ViewMatrix = CameraComponent->GetViewMatrix();
        FMatrix4 ProjectionMatrix = CameraComponent->GetProjectionMatrix();
        ProjectionMatrix[1][1] *= -1.0f;

        const ImVec2 ViewportOrigin = ImGui::GetCursorScreenPos();

        ImGuizmo::SetDrawlist(ImGui::GetCurrentWindow()->DrawList);
        ImGuizmo::SetRect(ViewportOrigin.x, ViewportOrigin.y, ViewportSize.x, ViewportSize.y);

        // Drop-target on the viewport: dropping a content-browser asset onto empty space spawns it
        // under the prefab root. Mirrors the world editor's behavior.
        {
            const ImRect ViewportRect(ViewportOrigin, ImVec2(ViewportOrigin.x + ViewportSize.x, ViewportOrigin.y + ViewportSize.y));
            if (ImGui::BeginDragDropTargetCustom(ViewportRect, ImGui::GetCurrentWindow()->ID))
            {
                const DragDrop::FPayload* Peek = DragDrop::PeekPayload();
                if (Peek && Peek->Kind == DragDrop::EPayloadKind::Asset && DragDrop::IsDelivered())
                {
                    HandlePrefabContentDrop(FStringView(Peek->AssetPath.c_str(), Peek->AssetPath.size()), entt::null, /*bAttachToTarget*/ false);
                }
                ImGui::EndDragDropTarget();
            }
        }

        // Viewport entity picking: left-click a prefab entity to select it (Ctrl-click toggles).
        // Selects the actual clicked entity (children included), no instance-root resolution here.
        if (bViewportHovered)
        {
            IRenderScene* Renderer = World->GetRenderer();
            if (Renderer != nullptr)
            {
                const uint32 PickerWidth  = Renderer->GetRenderExtent().x;
                const uint32 PickerHeight = Renderer->GetRenderExtent().y;

                const ImVec2 WinPos = ImGui::GetWindowPos();
                const ImVec2 Mouse  = ImGui::GetMousePos();
                const float LocalX = Math::Clamp(Mouse.x - WinPos.x, 0.0f, ViewportSize.x - 1.0f);
                const float LocalY = Math::Clamp(Mouse.y - WinPos.y, 0.0f, ViewportSize.y - 1.0f);
                const uint32 TexX = static_cast<uint32>(LocalX * static_cast<float>(PickerWidth)  / ViewportSize.x);
                const uint32 TexY = static_cast<uint32>(LocalY * static_cast<float>(PickerHeight) / ViewportSize.y);

                // Publish the cursor so the renderer reads back the pixel under it.
                Renderer->SetPickerCursor(TexX, TexY, true);

                // Skip picking while the mouse is on the camera-preview resize grip (flag set by
                // DrawCameraPreviewOverlay below, one frame behind).
                const bool bOverGizmo = (bImGuizmoUsedOnce && ImGuizmo::IsOver()) || ImGuizmo::IsUsing();
                if (!bOverGizmo && !bCameraPreviewMouseOver && !ShouldSuppressViewportClickInput()
                    && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    entt::entity Hit = Renderer->GetEntityAtPixel(TexX, TexY);
                    entt::registry& Registry = ECS::GetWorldRegistry(*World);
                    if (Hit != entt::null && Registry.valid(Hit) && Registry.any_of<SPrefabComponent>(Hit))
                    {
                        if (ImGui::GetIO().KeyCtrl)
                        {
                            ToggleSelectedEntity(Hit);
                        }
                        else
                        {
                            SetSingleSelectedEntity(Hit);
                        }
                    }
                }
            }
        }

        // Selected-camera PiP (shared with the world editor); before the gizmo's early returns.
        DrawCameraPreviewOverlay(ViewportOrigin, ViewportSize);

        const entt::entity PivotEntity = GetLastSelectedEntity();
        const bool bGizmoTargetValid = PivotEntity != entt::null && World->IsValidEntity(PivotEntity);

        // Mid-drag selection vanish: end the transaction so future clicks aren't blocked by IsOver().
        if (!bGizmoTargetValid && bImGuizmoUsedOnce)
        {
            EndTransaction("Transform");
            bImGuizmoUsedOnce = false;
        }

        if (!bGizmoTargetValid)
        {
            return;
        }

        STransformComponent* PivotTransform = World->TryGetComponent<STransformComponent>(PivotEntity);
        if (PivotTransform == nullptr)
        {
            return;
        }

        FMatrix4 EntityMatrix = PivotTransform->GetWorldMatrix();
        FMatrix4 PreManipulate = EntityMatrix;

        float* SnapValues = nullptr;
        float SnapArray[3] = {};
        if (bGuizmoSnapEnabled)
        {
            switch (GuizmoOp)
            {
            case ImGuizmo::TRANSLATE:
                SnapArray[0] = SnapArray[1] = SnapArray[2] = GuizmoSnapTranslate;
                SnapValues = SnapArray;
                break;
            case ImGuizmo::ROTATE:
                SnapArray[0] = SnapArray[1] = SnapArray[2] = GuizmoSnapRotate;
                SnapValues = SnapArray;
                break;
            case ImGuizmo::SCALE:
                SnapArray[0] = SnapArray[1] = SnapArray[2] = GuizmoSnapScale;
                SnapValues = SnapArray;
                break;
            }
        }

        // A camera gesture (or Alt merely held) must never grab the gizmo. Enable(false) leaves it
        // drawn but inert; never applied mid-drag, since it clears ImGuizmo's using state without a
        // release and would strand the open transaction.
        const bool bGizmoInert = ShouldSuppressViewportClickInput() && !bImGuizmoUsedOnce;
        if (bGizmoInert)
        {
            ImGuizmo::Enable(false);
        }

        ImGuizmo::Manipulate(Math::ValuePtr(ViewMatrix), Math::ValuePtr(ProjectionMatrix),
            GuizmoOp, GuizmoMode, Math::ValuePtr(EntityMatrix), nullptr, SnapValues);

        if (bGizmoInert)
        {
            ImGuizmo::Enable(true);
        }

        if (ImGuizmo::IsUsing())
        {
            if (!bImGuizmoUsedOnce)
            {
                // Transform-only; see WorldEditorTool for why this is not the whole-registry snapshot.
                // The pivot is driven directly and the rest co-move, so the record is both.
                TVector<entt::entity> Dragged;
                Dragged.reserve(SelectedEntities.size() + 1);
                Dragged.push_back(PivotEntity);
                for (entt::entity Selected : SelectedEntities)
                {
                    if (Selected != PivotEntity)
                    {
                        Dragged.push_back(Selected);
                    }
                }
                BeginTransformTransaction(Dragged);

                bImGuizmoUsedOnce = true;
            }

            entt::registry& Registry = ECS::GetWorldRegistry(*World);

            // Apply the same world translation/rotation to every selected entity (rigid group);
            // scale stays per-entity to avoid skew under mixed parent transforms.
            FMatrix4 DeltaWorld = EntityMatrix * Math::Inverse(PreManipulate);
            FVector3 DeltaTranslation, DeltaScale, DeltaSkew;
            FQuat DeltaRotation;
            FVector4 DeltaPersp;
            Math::Decompose(DeltaWorld, DeltaScale, DeltaRotation, DeltaTranslation, DeltaSkew, DeltaPersp);

            // Pivot itself: drive it directly with the manipulator's full output.
            EditorEntityUtils::ApplyWorldMatrixToTransform(Registry, PivotEntity, EntityMatrix);

            // Co-move every other selected entity by the pivot's delta. Skip locked-prefab-style
            // children, prefab editor has no locked instances, so the only filter is "valid + not pivot".
            const FVector3 PivotPreLocation = FVector3(PreManipulate[3]);
            for (entt::entity Other : SelectedEntities)
            {
                if (Other == PivotEntity || !Registry.valid(Other))
                {
                    continue;
                }
                STransformComponent* OtherTransform = Registry.try_get<STransformComponent>(Other);
                if (OtherTransform == nullptr)
                {
                    continue;
                }

                const FMatrix4 OtherWorld = OtherTransform->GetWorldMatrix();
                FMatrix4 NewWorld = OtherWorld;

                switch (GuizmoOp)
                {
                case ImGuizmo::TRANSLATE:
                    NewWorld[3] = FVector4(FVector3(OtherWorld[3]) + DeltaTranslation, 1.0f);
                    break;
                case ImGuizmo::ROTATE:
                {
                    const FVector3 OffsetFromPivot = FVector3(OtherWorld[3]) - PivotPreLocation;
                    const FVector3 RotatedOffset   = DeltaRotation * OffsetFromPivot;
                    NewWorld = Math::Translate(FMatrix4(1.f), PivotPreLocation + RotatedOffset)
                             * Math::ToMatrix4(DeltaRotation)
                             * FMatrix4(FMatrix3(OtherWorld));
                    break;
                }
                case ImGuizmo::SCALE:
                {
                    const FVector3 OffsetFromPivot = FVector3(OtherWorld[3]) - PivotPreLocation;
                    const FVector3 ScaledOffset    = OffsetFromPivot * DeltaScale;
                    FQuat OtherRot;
                    FVector3 OtherTr, OtherSc, OtherSk;
                    FVector4 OtherPe;
                    Math::Decompose(OtherWorld, OtherSc, OtherRot, OtherTr, OtherSk, OtherPe);
                    NewWorld = Math::Translate(FMatrix4(1.f), PivotPreLocation + ScaledOffset)
                             * Math::ToMatrix4(OtherRot)
                             * Math::Scale(FMatrix4(1.f), OtherSc * DeltaScale);
                    break;
                }
                default: break;
                }

                EditorEntityUtils::ApplyWorldMatrixToTransform(Registry, Other, NewWorld);
            }

            Asset->GetPackage()->MarkDirty();
        }
        else if (bImGuizmoUsedOnce)
        {
            EndTransaction("Transform");
            bImGuizmoUsedOnce = false;
        }
    }

    void FPrefabEditorTool::PersistGizmoSettings()
    {
        CPrefabEditorSettings* Settings = GetMutableDefault<CPrefabEditorSettings>();
        Settings->bGizmoSnapEnabled  = bGuizmoSnapEnabled;
        Settings->GizmoSnapTranslate = GuizmoSnapTranslate;
        Settings->GizmoSnapRotate    = GuizmoSnapRotate;
        Settings->GizmoSnapScale     = GuizmoSnapScale;
        GConfig->SaveSettings(CPrefabEditorSettings::StaticClass());
    }

    void FPrefabEditorTool::PersistCameraPreviewScale()
    {
        CPrefabEditorSettings* Settings = GetMutableDefault<CPrefabEditorSettings>();
        Settings->CameraPreviewScale = CameraPreviewScale;
        GConfig->SaveSettings(CPrefabEditorSettings::StaticClass());
    }

    void FPrefabEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Authoring",
            "Build the prefab in this isolated preview world like you would a normal scene. "
            "Saving commits the entity hierarchy back to the prefab asset.");
        DrawHelpTextRow("Selection / Gizmo",
            "Same controls as the world editor: W/E/R for translate/rotate/scale, X for World/Local, "
            "Ctrl-click multi-select, F frames the selection.");
        DrawHelpTextRow("Components",
            "Add Component on a selected entity to attach. Drag an asset directly onto the entity row "
            "in the outliner for shortcut adds (e.g. drop a static mesh to add a SStaticMeshComponent).");
        DrawHelpTextRow("Nested Prefabs",
            "Drag another prefab asset into the outliner to instance it as a child. Property overrides "
            "are per-instance; structural changes happen on the source prefab.");
        DrawHelpTextRow("Save",
            "Ctrl+S commits all entities, components and overrides to the prefab. Existing instances "
            "in worlds reload on next open or with Reload Asset.");
    }

    void FPrefabEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);

        if (ImGui::BeginMenu(LE_ICON_MOVE_RESIZE " Gizmo"))
        {
            const char* Ops[] = { "Translate", "Rotate", "Scale" };
            int Current = (GuizmoOp == ImGuizmo::TRANSLATE) ? 0 : (GuizmoOp == ImGuizmo::ROTATE ? 1 : 2);
            if (ImGui::Combo("##GizmoOp", &Current, Ops, IM_ARRAYSIZE(Ops)))
            {
                switch (Current)
                {
                case 0: GuizmoOp = ImGuizmo::TRANSLATE; break;
                case 1: GuizmoOp = ImGuizmo::ROTATE;    break;
                case 2: GuizmoOp = ImGuizmo::SCALE;     break;
                }
            }

            const char* Modes[] = { "World", "Local" };
            int ModeIdx = (GuizmoMode == ImGuizmo::WORLD) ? 0 : 1;
            if (ImGui::Combo("##GizmoMode", &ModeIdx, Modes, IM_ARRAYSIZE(Modes)))
            {
                GuizmoMode = (ModeIdx == 0) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
            }

            ImGui::Checkbox("Snap", &bGuizmoSnapEnabled);
            ImGui::DragFloat("Translate Step", &GuizmoSnapTranslate, 0.01f, 0.001f, 100.0f);
            ImGui::DragFloat("Rotate Step (deg)", &GuizmoSnapRotate, 0.5f, 0.1f, 90.0f);
            ImGui::DragFloat("Scale Step", &GuizmoSnapScale, 0.01f, 0.001f, 10.0f);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LE_ICON_EYE " View"))
        {
            ImGui::Checkbox("World Grid", &bWorldGridEnabled);
            ImGui::Checkbox("Component Visualizers", &bShowComponentVisualizers);
            if (ImGui::MenuItem(LE_ICON_HOME " Frame All", "Home"))
            {
                FrameAllEntities();
            }
            if (ImGui::MenuItem(LE_ICON_TARGET " Focus Selection", "F"))
            {
                FocusViewportToEntity(GetLastSelectedEntity());
            }
            ImGui::EndMenu();
        }
    }

    void FPrefabEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftDockID = 0, RightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.3f, &RightDockID, &LeftDockID);

        ImGuiID LeftOutlinerID = 0, LeftViewportID = 0;
        ImGui::DockBuilderSplitNode(LeftDockID, ImGuiDir_Left, 0.25f, &LeftOutlinerID, &LeftViewportID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), LeftViewportID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(OutlinerWindowName).c_str(), LeftOutlinerID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(PropertiesWindowName).c_str(), RightDockID);
    }
}
