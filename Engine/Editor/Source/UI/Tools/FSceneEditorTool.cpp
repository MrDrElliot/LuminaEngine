#include "FSceneEditorTool.h"
#include "Scripting/DotNet/DotNetHost.h"

#include <execution>

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "EditorToolContext.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Components/EditorEntityTags.h"
#include "Config/Config.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Math/Math.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "EASTL/sort.h"
#include "Input/InputViewport.h"
#include "Renderer/ViewVolume.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Traits.h"
#include "Animation/SkeletalMeshUtils.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Tools/ComponentVisualizers/ComponentVisualizer.h"
#include "Tools/Dialogs/Dialogs.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "UI/Properties/EntityPropertyContext.h"
#include "UI/Tools/EditorEntityUtils.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/CSharpScriptComponent.h"
#include "World/Entity/Components/TagComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/EntityUtils.h"
#include "World/World.h"
#include "World/Entity/Components/LightComponent.h"

namespace Lumina
{
    // Route through the asset-tool's display-name-only ctor so the inherited PropertyTable
    // stays unbound (neither subclass uses it; they drive their own per-component tables).
    // Asset + World are assigned directly; FEditorTool's ctor only stores World, so setting
    // it post-construction is safe.
    FSceneEditorTool::FSceneEditorTool(IEditorToolContext* Context, const FString& DisplayName, CObject* InAsset, CWorld* InWorld)
        : FAssetEditorTool(Context, DisplayName)
    {
        Asset = InAsset;
        World = InWorld;
    }

    FSceneEditorTool::FSceneEditorTool(IEditorToolContext* Context, const FString& DisplayName, CWorld* InWorld)
        : FAssetEditorTool(Context, DisplayName)
    {
        // World path: the live CWorld is the document, but it is NOT held as the FAssetEditorTool
        // Asset, its lifetime is owned by the editor (created/swapped/destroyed elsewhere), so a
        // second owning ref here would corrupt the refcount on swap. Asset stays null; saving and
        // dirtying go through GetScenePackage()/OnSave overrides.
        World = InWorld;
    }

    void FSceneEditorTool::OnSave()
    {
        CommitScene();
        FAssetEditorTool::OnSave();
    }

    void FSceneEditorTool::OnAssetLoadFinished()
    {
        OnSceneLoaded();
    }

    CPackage* FSceneEditorTool::GetScenePackage() const
    {
        return Asset != nullptr ? Asset->GetPackage() : nullptr;
    }

    void FSceneEditorTool::MarkSceneDirty()
    {
        if (CPackage* Package = GetScenePackage())
        {
            Package->MarkDirty();
        }
    }

    void FSceneEditorTool::SetSingleSelectedEntity(entt::entity Entity)
    {
        FEntityRegistry& Registry = GetSceneRegistry();
        if (Entity != entt::null && !Registry.valid(Entity))
        {
            Entity = entt::null;
        }

        // Fast-path: clicking the already-singularly-selected entity is a no-op.
        if (Entity == LastSelectedEntity && SelectedEntities.size() == (Entity == entt::null ? 0 : 1)
            && (Entity == entt::null || SelectedEntities.find(Entity) != SelectedEntities.end()))
        {
            return;
        }

        // Drop tags from entities no longer selected so render highlighting matches the canonical set.
        for (entt::entity Old : SelectedEntities)
        {
            if (Old != Entity && Registry.valid(Old))
            {
                Registry.remove<FSelectedInEditorComponent>(Old);
                SyncOutlinerRowSelection(Old, false);
            }
        }
        SelectedEntities.clear();

        // Clear last-selected tag unconditionally; re-emplace below if new selection isn't empty.
        Registry.clear<FLastSelectedTag>();

        if (Entity != entt::null)
        {
            SelectedEntities.insert(Entity);
            Registry.emplace_or_replace<FSelectedInEditorComponent>(Entity);
            Registry.emplace_or_replace<FLastSelectedTag>(Entity);
            SyncOutlinerRowSelection(Entity, true);
        }

        if (LastSelectedEntity != Entity)
        {
            LastSelectedEntity = Entity;
            OnSelectionChanged();
        }
    }

    void FSceneEditorTool::AddSelectedEntity(entt::entity Entity, bool /*bRebuild*/)
    {
        FEntityRegistry& Registry = GetSceneRegistry();
        if (Entity == entt::null || !Registry.valid(Entity))
        {
            return;
        }

        const bool bWasAlreadySelected = SelectedEntities.find(Entity) != SelectedEntities.end();
        if (!bWasAlreadySelected)
        {
            SelectedEntities.insert(Entity);
            Registry.emplace_or_replace<FSelectedInEditorComponent>(Entity);
            SyncOutlinerRowSelection(Entity, true);
        }

        // Always promote to last-selected so clicking a row in a multi-select focuses details.
        if (LastSelectedEntity != Entity)
        {
            Registry.clear<FLastSelectedTag>();
            Registry.emplace_or_replace<FLastSelectedTag>(Entity);
            LastSelectedEntity = Entity;
            OnSelectionChanged();
        }
    }

    void FSceneEditorTool::RemoveSelectedEntity(entt::entity Entity, bool /*bRebuild*/)
    {
        if (Entity == entt::null)
        {
            return;
        }

        auto SetIt = SelectedEntities.find(Entity);
        if (SetIt == SelectedEntities.end())
        {
            return;
        }

        SelectedEntities.erase(SetIt);

        FEntityRegistry& Registry = GetSceneRegistry();
        if (Registry.valid(Entity))
        {
            Registry.remove<FSelectedInEditorComponent>(Entity);
        }

        SyncOutlinerRowSelection(Entity, false);

        // If the deselected entity was the focus, pick a new one so "last" isn't stale.
        if (LastSelectedEntity == Entity)
        {
            Registry.clear<FLastSelectedTag>();
            entt::entity NewLast = entt::null;
            for (entt::entity Candidate : SelectedEntities)
            {
                if (Registry.valid(Candidate))
                {
                    NewLast = Candidate;
                    break;
                }
            }
            if (NewLast != entt::null)
            {
                Registry.emplace_or_replace<FLastSelectedTag>(NewLast);
            }
            LastSelectedEntity = NewLast;
            OnSelectionChanged();
        }
    }

    void FSceneEditorTool::ToggleSelectedEntity(entt::entity Entity)
    {
        FEntityRegistry& Registry = GetSceneRegistry();
        if (Entity == entt::null || !Registry.valid(Entity))
        {
            return;
        }

        if (SelectedEntities.find(Entity) != SelectedEntities.end())
        {
            RemoveSelectedEntity(Entity, false);
        }
        else
        {
            AddSelectedEntity(Entity, false);
        }
    }

    void FSceneEditorTool::ResyncSelectionFromRegistry()
    {
        // Clear old outliner row state; re-mark below from the post-resync set.
        for (entt::entity Old : SelectedEntities)
        {
            SyncOutlinerRowSelection(Old, false);
        }
        SelectedEntities.clear();
        LastSelectedEntity = entt::null;

        FEntityRegistry& Registry = GetSceneRegistry();

        Registry.view<FSelectedInEditorComponent>().each([&](entt::entity Entity)
        {
            SelectedEntities.insert(Entity);
            SyncOutlinerRowSelection(Entity, true);
        });

        // FLastSelectedTag should be serialized; fall back to first selected if it's missing.
        Registry.view<FLastSelectedTag>().each([&](entt::entity Entity)
        {
            LastSelectedEntity = Entity;
        });

        if (LastSelectedEntity == entt::null && !SelectedEntities.empty())
        {
            entt::entity First = *SelectedEntities.begin();
            LastSelectedEntity = First;
            Registry.emplace_or_replace<FLastSelectedTag>(First);
        }

        OnSelectionChanged();
    }

    void FSceneEditorTool::ReapplySelectionTags()
    {
        FEntityRegistry& Registry = GetSceneRegistry();
        for (entt::entity Entity : SelectedEntities)
        {
            if (Registry.valid(Entity))
            {
                Registry.emplace_or_replace<FSelectedInEditorComponent>(Entity);
            }
        }
        if (LastSelectedEntity != entt::null && Registry.valid(LastSelectedEntity))
        {
            Registry.emplace_or_replace<FLastSelectedTag>(LastSelectedEntity);
        }
    }

    void FSceneEditorTool::ClearSelectedEntities()
    {
        FEntityRegistry& Registry = GetSceneRegistry();

        for (entt::entity Entity : SelectedEntities)
        {
            SyncOutlinerRowSelection(Entity, false);
        }

        SelectedEntities.clear();

        // Bulk-erase via registry clear<>(); cheaper than walking SelectedEntities.
        Registry.clear<FSelectedInEditorComponent>();
        Registry.clear<FLastSelectedTag>();

        if (LastSelectedEntity != entt::null)
        {
            LastSelectedEntity = entt::null;
            OnSelectionChanged();
        }
    }

    bool FSceneEditorTool::IsComponentHiddenInDetails(const CStruct* Type) const
    {
        // Deliberately not HideInComponentList, which means "cannot be hand-added" and is a different
        // question: SFoliageComponent carries it so the picker will not offer it, while its settings
        // are exactly what the details panel is for.
        return Type != nullptr && Type->HasMeta("HideInDetails");
    }

    void FSceneEditorTool::DrawComponentSearchBar()
    {
        // Clearing needs no special handling here: DrawComponentList pushes the (now empty) text into
        // every component table each frame, and each table restores its own collapse state when its
        // filter goes inactive.
        ImGuiX::SearchBar("##DetailsSearch", DetailsFilter, LE_ICON_MAGNIFY " Search components and properties...");

        ImGui::Spacing();
    }

    void FSceneEditorTool::DrawComponentList(entt::entity Entity)
    {
        DrawComponentSearchBar();

        const bool bFiltering = DetailsFilter.IsActive();
        uint32 VisibleCount = 0;

        for (FComponentTableEntry& Entry : PropertyTables)
        {
            if (bFiltering)
            {
                // A component whose NAME matches shows in full -- searching "Static Mesh" means you want
                // that component, not the subset of its properties that happen to repeat the word.
                const bool bTitleMatches = DetailsFilter.PassFilter(Entry.Title.c_str());

                if (Entry.Table)
                {
                    Entry.Table->SetSearchText(bTitleMatches ? FStringView() : FStringView(DetailsFilter.InputBuf));

                    // Skip the whole component when nothing inside it survives. Drawing the header anyway
                    // would leave a list of empty sections to scroll past, which is the thing a search is
                    // meant to remove.
                    if (!bTitleMatches && !Entry.Table->PrepareAndTestFilter())
                    {
                        continue;
                    }
                }
                else if (!bTitleMatches)
                {
                    continue;
                }
            }
            else if (Entry.Table)
            {
                Entry.Table->SetSearchText(FStringView());
            }

            DrawComponentHeader(Entry, Entity);
            ImGui::Spacing();
            ++VisibleCount;
        }

        if (bFiltering && VisibleCount == 0)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("   No components or properties match.");
        }
    }

    void FSceneEditorTool::DrawComponentHeader(FComponentTableEntry& Entry, entt::entity Entity)
    {
        using namespace entt::literals;

        // Existence check via meta; drop this row if the entity no longer holds the component.
        if (IsComponentHiddenInDetails(Entry.ReflectedType))
        {
            return;
        }

        entt::meta_type MetaType = entt::resolve(entt::hashed_string(Entry.ReflectedType->GetName().c_str()));
        if (!ECS::Utils::HasComponent(GetSceneRegistry(), Entity, MetaType))
        {
            return;
        }

        const bool bIsRequired =
            Entry.ReflectedType == STransformComponent::StaticStruct() || Entry.ReflectedType == SNameComponent::StaticStruct();

        ImGui::PushID(&Entry);

        constexpr ImGuiTableFlags Flags =
        ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_NoBordersInBodyUntilResize |
        ImGuiTableFlags_SizingFixedFit;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 10.0f)); // increase Y for taller header
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
        bool bIsOpen = false;
        if (ImGui::BeginTable("GridTable", 1, Flags))
        {
            ImGui::TableSetupColumn("##Header", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();

            ImGui::PushStyleColor(ImGuiCol_Header, EditorColors::U32(EditorColors::Header()));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorColors::U32(EditorColors::RowBgHovered()));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorColors::U32(EditorColors::RowBgActive()));
            ImGui::SetNextItemAllowOverlap();
            bIsOpen = ImGui::CollapsingHeader(Entry.Title.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, EditorColors::U32(EditorColors::PanelBg()));

            ImGui::PopStyleColor(3);

            if (!bIsRequired)
            {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 28.0f);

                ImGui::PushStyleColor(ImGuiCol_Button, EditorColors::WithAlpha(EditorColors::Danger(), 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorColors::WithAlpha(EditorColors::Danger(), 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorColors::Danger());
                ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Danger());
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

                if (ImGui::SmallButton(LE_ICON_TRASH_CAN "##RemoveComponent"))
                {
                    ComponentDestroyRequests.push(FComponentDestroyRequest{ Entry.ReflectedType, Entity });
                }

                ImGuiX::TextTooltip("{}", "Remove Component");

                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);
            }

            ImGui::EndTable();
        }

        ImGui::PopStyleVar(2);


        if (bIsOpen && Entry.Table != nullptr)
        {
            ImGui::Spacing();

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 6.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorColors::FrameBg());
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, EditorColors::Lighten(EditorColors::FrameBg(), 0.1f));

            ImGui::Indent(8.0f);

            // Make this component's world resolvable to any PROPERTY(Entity) picker in the table, and
            // the parent's mesh sockets/bones to any SocketPicker FName (e.g. SSocketAttachmentComponent).
            {
                SocketPickerContext::FSocketPickerData SocketData;
                BuildSocketPickerData(Entity, SocketData);

                FScopedEntityPropertyContext EntityContext(World);
                SocketPickerContext::FScope SocketScope(&SocketData);
                Entry.Table->DrawTree();
            }

            ImGui::Unindent(8.0f);

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);

            ImGui::Spacing();
        }

        ImGui::PopID();
    }

    void FSceneEditorTool::BuildSocketPickerData(entt::entity Entity, SocketPickerContext::FSocketPickerData& Out)
    {
        FEntityRegistry& Registry = GetSceneRegistry();

        const FRelationshipComponent* Relationship = Registry.try_get<FRelationshipComponent>(Entity);
        if (Relationship == nullptr || Relationship->Parent == entt::null || !Registry.valid(Relationship->Parent))
        {
            return;
        }

        if (const SSkeletalMeshComponent* SkeletalMesh = Registry.try_get<SSkeletalMeshComponent>(Relationship->Parent))
        {
            if (CSkeleton* Skeleton = SkeletalUtils::GetSkeletonAsset(*SkeletalMesh))
            {
                for (const FMeshSocket& Socket : Skeleton->Sockets)
                {
                    Out.Sockets.push_back(Socket.SocketName);
                }
                Out.Skeleton = Skeleton->GetSkeletonResource();
            }
        }
        else if (const SStaticMeshComponent* StaticMesh = Registry.try_get<SStaticMeshComponent>(Relationship->Parent))
        {
            if (StaticMesh->StaticMesh.IsValid())
            {
                for (const FMeshSocket& Socket : StaticMesh->StaticMesh->Sockets)
                {
                    Out.Sockets.push_back(Socket.SocketName);
                }
            }
        }
    }

    void FSceneEditorTool::RemoveComponent(entt::entity Entity, const CStruct* ComponentType)
    {
        bool bWasRemoved = false;

        if (ComponentType == nullptr)
        {
            return;
        }

        ECS::Utils::ForEachComponent(GetSceneRegistry(), Entity, [&](void* Component, entt::basic_sparse_set<>& Set, const entt::meta_type& Type)
        {
            using namespace entt::literals;

            if (entt::meta_any ReturnValue = ECS::Utils::InvokeMetaFunc(Type, "static_struct"_hs))
            {
                CStruct* StructType = ReturnValue.cast<CStruct*>();

                if (StructType == ComponentType)
                {
                    Set.remove(Entity);
                    bWasRemoved = true;
                }
            }
        });


        if (bWasRemoved)
        {
            // Prefab instance: record the removal so a refresh won't re-add an inherited component (no-op
            // for non-instance entities). The prefab asset is untouched, so the inherited/added test is valid.
            CPrefab::NoteComponentRemoved(GetSceneRegistry(), Entity, const_cast<CStruct*>(ComponentType));

            // Mark dirty; next DrawEntityEditor pass rebuilds PropertyTables. Avoids tearing down handles mid-draw.
            if (Entity == DetailsEntity)
            {
                bDetailsDirty = true;
            }
            MarkSceneDirty();
        }
        else
        {
            ImGuiX::Notifications::NotifyError("Failed to remove component: {0}", ComponentType->GetName().c_str());
        }
    }

    void FSceneEditorTool::ProcessComponentEditRequests()
    {
        if (!ComponentDestroyRequests.empty())
        {
            BeginTransaction();
            while (!ComponentDestroyRequests.empty())
            {
                FComponentDestroyRequest Request = ComponentDestroyRequests.front();
                ComponentDestroyRequests.pop();

                RemoveComponent(Request.EntityID, Request.Type);
            }
            EndTransaction("Remove Component");
        }
    }

    void FSceneEditorTool::OnPrePropertyChangeEvent(const FPropertyChangedEvent& Event)
    {
    }

    void FSceneEditorTool::OnPostPropertyChangeEvent(const FPropertyChangedEvent& Event)
    {
        using namespace entt::literals;

        // A reset-to-default (and some nested rows) can fire with no outer component type; there's
        // nothing to resolve/patch then, so bail before dereferencing it.
        if (Event.OuterType == nullptr)
        {
            return;
        }

        FEntityRegistry& Registry = GetSceneRegistry();
        entt::id_type TypeID = ECS::Utils::GetTypeID(Event.OuterType->GetName().c_str());
        entt::meta_type WantType = entt::resolve(TypeID);
        if (!WantType)
        {
            return;
        }

        // Multi-edit source: the focus entity is what the bound property table edited. Locate its
        // component instance so the change can be replicated onto the rest of the selection.
        void* FocusInstance = nullptr;
        if (Event.Property != nullptr && Registry.valid(DetailsEntity))
        {
            ECS::Utils::ForEachComponent(Registry, DetailsEntity, [&](void* Component, entt::basic_sparse_set<>&, const entt::meta_type& Type)
            {
                if (FocusInstance == nullptr && Type == WantType)
                {
                    FocusInstance = Component;
                }
            });
        }

        auto View = Registry.view<FSelectedInEditorComponent>();
        View.each([&](entt::entity Entity)
        {
            entt::meta_any Has = ECS::Utils::InvokeMetaFunc(TypeID, "has"_hs, entt::forward_as_meta(Registry), Entity);
            if (!Has || !Has.cast<bool>())
            {
                return;
            }

            // Copy the edited property from the focus instance onto this one.
            if (FocusInstance != nullptr && Entity != DetailsEntity)
            {
                void* DestInstance = nullptr;
                ECS::Utils::ForEachComponent(Registry, Entity, [&](void* Component, entt::basic_sparse_set<>&, const entt::meta_type& Type)
                {
                    if (DestInstance == nullptr && Type == WantType)
                    {
                        DestInstance = Component;
                    }
                });

                if (DestInstance != nullptr && DestInstance != FocusInstance)
                {
                    Event.Property->CopyCompleteValue_InContainer(DestInstance, FocusInstance);
                }
            }

            // Prefab instance: re-diff this component against the prefab so the override ledger reflects the
            // edit. A back-to-prefab edit (or a reset, which dispatches as an edit) leaves zero divergent
            // leaves, clearing the record. No-op for non-instance entities.
            CPrefab::RecaptureComponentOverrides(Registry, Entity, Event.OuterType);
        });
    }

    void FSceneEditorTool::RebuildPropertyTables(entt::entity Entity)
    {
        using namespace entt::literals;

        PropertyTables.clear();

        // Track owning entity so the details panel can detect staleness; null on invalid input forces a rebuild next time.
        DetailsEntity = (Entity != entt::null && GetSceneRegistry().valid(Entity)) ? Entity : entt::null;
        bDetailsDirty = false;

        if (GetSceneRegistry().valid(Entity))
        {
            FEntityRegistry& Registry = GetSceneRegistry();

            // One intermediate row per reflected component.
            struct FPendingRow
            {
                void*                 Data = nullptr;
                CStruct*              Layout = nullptr;        // reflected CStruct
                const CStruct*        ReflectedType = nullptr;
                FString               Title;
                TVector<void*>        OtherInstances;          // same component on the other selected entities (multi-edit)
            };

            // Multi-edit: when more than one entity is selected, the details panel shows the intersection
            // of components and compares each property's value across the whole selection.
            const bool bMultiSelect = SelectedEntities.size() > 1 && IsEntitySelected(Entity);

            TVector<entt::entity> OtherTargets;
            TVector<THashMap<const CStruct*, void*>> OtherReflected;
            if (bMultiSelect)
            {
                for (entt::entity Selected : SelectedEntities)
                {
                    if (Selected == Entity || !Registry.valid(Selected))
                    {
                        continue;
                    }
                    OtherTargets.push_back(Selected);

                    THashMap<const CStruct*, void*> ReflectedMap;
                    ECS::Utils::ForEachComponent(Registry, Selected, [&](void* Component, entt::basic_sparse_set<>&, const entt::meta_type& Type)
                    {
                        entt::meta_any Any = ECS::Utils::InvokeMetaFunc(Type, "static_struct"_hs);
                        if (Any)
                        {
                            ReflectedMap[Any.cast<CStruct*>()] = Component;
                        }
                    });
                    OtherReflected.push_back(Move(ReflectedMap));
                }
            }

            TVector<FPendingRow> Pending;

            ECS::Utils::ForEachComponent(Registry, Entity, [&](void* Component, entt::basic_sparse_set<>& Set, const entt::meta_type& Type)
            {
                entt::meta_any Any = ECS::Utils::InvokeMetaFunc(Type, "static_struct"_hs);
                if (!Any)
                {
                    return;
                }

                CStruct* Struct = Any.cast<CStruct*>();

                // Components that never show in the details panel (tags, prefab bookkeeping).
                if (IsComponentHiddenInDetails(Struct))
                {
                    return;
                }

                // Intersection: drop components not present on every selected entity, gathering the
                // matching instance pointers for the multi-value compare.
                TVector<void*> Others;
                for (const THashMap<const CStruct*, void*>& Map : OtherReflected)
                {
                    auto It = Map.find(Struct);
                    if (It == Map.end())
                    {
                        return;
                    }
                    Others.push_back(It->second);
                }

                Pending.push_back({ Component, Struct, Struct, Struct->MakeDisplayName().c_str(), Move(Others) });
            });

            eastl::sort(Pending.begin(), Pending.end(), [&](const FPendingRow& LHS, const FPendingRow& RHS)
            {
                // Name first, Transform second, everything else alphabetical.
                auto Priority = [](const FPendingRow& Row) -> uint32
                {
                    if (Row.ReflectedType == SNameComponent::StaticStruct())      { return 0; }
                    if (Row.ReflectedType == STransformComponent::StaticStruct()) { return 1; }
                    return 2;
                };

                const uint32 APriority = Priority(LHS);
                const uint32 BPriority = Priority(RHS);
                if (APriority != BPriority)
                {
                    return APriority < BPriority;
                }
                return LHS.Title < RHS.Title;
            });

            // For a prefab instance, reset-to-default and the "modified" marker compare against the prefab
            // baseline (not the class CDO). Resolve the focus entity's instance info once.
            SPrefabInstanceComponent* FocusPrefabInstance = Registry.try_get<SPrefabInstanceComponent>(Entity);

            // Resolved once rather than per row, and screened the way CPrefab::CullOrphanedInstances
            // screens the same field: a source prefab can be a marked-destroy zombie, which a null
            // test does not catch, because TObjectPtr's Get/IsValid are plain pointer reads that
            // never consult GObjectArray.
            CPrefab* FocusSourcePrefab = FocusPrefabInstance != nullptr ? FocusPrefabInstance->SourcePrefab.Get() : nullptr;
            if (FocusSourcePrefab != nullptr && FocusSourcePrefab->HasAnyFlag(OF_MarkedDestroy))
            {
                FocusSourcePrefab = nullptr;
            }

            for (const FPendingRow& Row : Pending)
            {
                FComponentTableEntry Entry;
                Entry.ReflectedType = Row.ReflectedType;
                Entry.Title         = Row.Title;

                // Prefab instances diff/reset against the matched prefab component; everything else
                // falls back to the struct CDO (the 2-arg form).
                void* PrefabDefault = nullptr;
                if (FocusSourcePrefab != nullptr && Row.Layout != nullptr)
                {
                    PrefabDefault = FocusSourcePrefab->ResolvePrefabComponentPtr(FocusPrefabInstance->StableID, Row.Layout);
                }

                Entry.Table = (PrefabDefault != nullptr)
                    ? MakeUnique<FPropertyTable>(Row.Data, Row.Layout, PrefabDefault)
                    : MakeUnique<FPropertyTable>(Row.Data, Row.Layout);
                // The details panel owns ONE search box across every component (DrawComponentSearchBar);
                // a per-table box would put one above each component header.
                Entry.Table->SetShowSearchBar(false);
                Entry.Table->SetPreEditCallback([&](const FPropertyChangedEvent& Event)    { OnPrePropertyChangeEvent(Event); });
                Entry.Table->SetPostEditCallback([&](const FPropertyChangedEvent& Event)   { OnPostPropertyChangeEvent(Event); MarkSceneDirty(); });
                Entry.Table->SetStartEditCallback([&](const FPropertyChangedEvent& Event)  { BeginTransaction(); });
                Entry.Table->SetFinishEditCallback([&](const FPropertyChangedEvent& Event) { EndTransaction(Event.PropertyName); });

                // Multi-edit value compare; propagation handled in OnPostPropertyChangeEvent.
                if (!Row.OtherInstances.empty())
                {
                    void* FocusData = Row.Data;
                    TVector<void*> Others = Row.OtherInstances;
                    Entry.Table->ChangeEventCallbacks.IsMultiValueFn = [FocusData, Others](FProperty* Property) -> bool
                    {
                        for (void* Other : Others)
                        {
                            if (!Property->Identical_InContainer(FocusData, Other))
                            {
                                return true;
                            }
                        }
                        return false;
                    };
                }
                Entry.Table->MarkDirty();

                PropertyTables.emplace_back(Move(Entry));
            }
        }
    }

    bool FSceneEditorTool::IsOutlinerEntityVisible(entt::entity Entity) const
    {
        FEntityRegistry& Registry = GetSceneRegistry();
        return Registry.valid(Entity)
            && Registry.all_of<SNameComponent>(Entity)
            && !Registry.any_of<FHideInSceneOutliner>(Entity);
    }

    void FSceneEditorTool::SyncOutlinerRowSelection(entt::entity Entity, bool bSelected)
    {
        auto It = EntityToTreeNode.find(Entity);
        if (It != EntityToTreeNode.end() && OutlinerListView.IsValid(It->second))
        {
            OutlinerListView.Get<FTreeNodeState>(It->second).bSelected = bSelected;
        }
    }

    void FSceneEditorTool::RebuildSceneOutliner(FTreeListView& Tree)
    {
        // Incremental tree: rebuild just resets the map and re-adds roots. Children fill lazily on expand.
        EntityToTreeNode.clear();
        PendingOutlinerAdds.clear();
        PendingExpanderRefresh.clear();

        FEntityRegistry& Registry = GetSceneRegistry();
        auto View = Registry.view<SNameComponent>(entt::exclude<FHideInSceneOutliner>);
        
        TVector<entt::entity> Roots;
        Roots.reserve(View.size_hint());
        for (entt::entity Entity : View)
        {
            if (!IsOutlinerEntityVisible(Entity))
            {
                continue;
            }
            if (FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(Entity))
            {
                if (Rel->Parent != entt::null)
                {
                    continue;
                }
            }

            Roots.push_back(Entity);
        }

        std::sort(std::execution::par, Roots.begin(), Roots.end(), [&](entt::entity LHS, entt::entity RHS)
        {
            const FName& A = View.get<SNameComponent>(LHS).Name;
            const FName& B = View.get<SNameComponent>(RHS).Name;

            if (A != B)
            {
                return A < B;
            }
            
            return LHS < RHS;
        });

        for (entt::entity Root : Roots)
        {
            AddEntityToOutliner(Root);
        }
    }

    FTreeNodeID FSceneEditorTool::AddEntityToOutliner(entt::entity Entity)
    {
        FEntityRegistry& Registry = GetSceneRegistry();
        if (!IsOutlinerEntityVisible(Entity))
        {
            return InvalidTreeNode;
        }

        auto Existing = EntityToTreeNode.find(Entity);
        if (Existing != EntityToTreeNode.end())
        {
            return Existing->second;
        }

        // Attach under parent if it's already in the tree.
        FTreeNodeID ParentNode = InvalidTreeNode;
        if (FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(Entity))
        {
            if (Rel->Parent != entt::null)
            {
                auto ParentIt = EntityToTreeNode.find(Rel->Parent);
                if (ParentIt != EntityToTreeNode.end())
                {
                    ParentNode = ParentIt->second;
                }
                else
                {
                    return InvalidTreeNode;
                }
            }
        }

        SNameComponent& NameComponent = Registry.get<SNameComponent>(Entity);
        const SPrefabInstanceComponent* PrefabInstance = Registry.try_get<SPrefabInstanceComponent>(Entity);
        const bool bIsPrefabInstanceRoot = PrefabInstance != nullptr && PrefabInstance->bIsRoot;
        const bool bIsLockedPrefabChild = PrefabInstance != nullptr && !PrefabInstance->bIsRoot;

        FFixedString Name;
        if (bIsPrefabInstanceRoot)
        {
            Name.append(LE_ICON_PACKAGE_VARIANT_CLOSED).append(" ");
        }
        else if (bIsLockedPrefabChild)
        {
            Name.append(LE_ICON_LOCK).append(" ");
        }
        else
        {
            Name.append(LE_ICON_CUBE).append(" ");
        }
        Name.append(NameComponent.Name.c_str()).append(" (").append_convert(eastl::to_string(entt::to_integral(Entity)) + ")");

        FTreeNodeID ItemEntity = OutlinerListView.CreateNode(ParentNode, FStringView(Name.data(), Name.length()));
        EntityToTreeNode[Entity] = ItemEntity;

        FTreeNodeDisplay& Display = OutlinerListView.Get<FTreeNodeDisplay>(ItemEntity);

        // Tint the entity's box glyph with the editor accent so plain entities stand out in the outliner.
        if (!bIsPrefabInstanceRoot && !bIsLockedPrefabChild)
        {
            Display.IconText = LE_ICON_CUBE;
            Display.IconColor = EditorColors::EntityIcon();
        }

        // The rich hover tooltip (title + subtitle + component chips) is intentionally NOT built here:
        // the component scan is the dominant per-row cost, and tooltips only show on hover. Built lazily
        // by BuildEntityTooltip on first hover instead.

        Display.bShowDisabledIcon = true;
        Display.bAllowRenaming = !bIsLockedPrefabChild;

        // Per-entity script enable toggle: only shown when the entity carries a script.
        if (Registry.any_of<SScriptComponent>(Entity))
        {
            Display.bShowSecondaryIcon = true;
            Display.SecondaryIconOn    = LE_ICON_SCRIPT_TEXT;
            Display.SecondaryIconOff   = LE_ICON_SCRIPT_TEXT_OUTLINE;
            Display.SecondaryTooltip   = "Toggle this entity's script on/off (the entity stays active).";
        }

        OutlinerListView.EmplaceUserData<FEntityListViewItemData>(ItemEntity).Entity = Entity;

        if (Registry.any_of<FSelectedInEditorComponent>(Entity))
        {
            OutlinerListView.Get<FTreeNodeState>(ItemEntity).bSelected = true;
        }

        if (Registry.any_of<SDisabledTag>(Entity))
        {
            OutlinerListView.Get<FTreeNodeState>(ItemEntity).bDisabled = true;
        }

        if (Registry.any_of<SScriptDisabledTag>(Entity))
        {
            OutlinerListView.Get<FTreeNodeState>(ItemEntity).bSecondaryToggled = true;
        }

        // Only show an expander if the entity actually has child entities; lazy expansion populates them.
        const FRelationshipComponent* RelForChildren = Registry.try_get<FRelationshipComponent>(Entity);
        const bool bHasChildren = RelForChildren != nullptr && RelForChildren->Children > 0;
        OutlinerListView.MarkHasLazyChildren(ItemEntity, bHasChildren);

        return ItemEntity;
    }

    void FSceneEditorTool::BuildEntityTooltip(entt::entity Entity, FTreeNodeDisplay& Display)
    {
        if (Display.bTooltipBuilt)
        {
            return;
        }

        FEntityRegistry& Registry = GetSceneRegistry();
        if (!Registry.valid(Entity) || !Registry.all_of<SNameComponent>(Entity))
        {
            return;
        }

        const SNameComponent& NameComponent = Registry.get<SNameComponent>(Entity);
        const SPrefabInstanceComponent* PrefabInstance = Registry.try_get<SPrefabInstanceComponent>(Entity);
        const bool bIsPrefabInstanceRoot = PrefabInstance != nullptr && PrefabInstance->bIsRoot;
        const bool bIsLockedPrefabChild = PrefabInstance != nullptr && !PrefabInstance->bIsRoot;

        // Styled hover tooltip: title (type icon + name), a dim subtitle, then component chips.
        const char* TypeIcon = bIsPrefabInstanceRoot ? LE_ICON_PACKAGE_VARIANT_CLOSED
                             : bIsLockedPrefabChild   ? LE_ICON_LOCK
                                                      : LE_ICON_CUBE;
        Display.TooltipTitle = FString(TypeIcon) + " " + NameComponent.Name.c_str();

        if (bIsLockedPrefabChild)
        {
            Display.TooltipSubtitle = FString("Prefab child #" + eastl::to_string(entt::to_integral(Entity)) + ", hierarchy locked");
        }
        else if (bIsPrefabInstanceRoot)
        {
            Display.TooltipSubtitle = FString("Prefab instance #" + eastl::to_string(entt::to_integral(Entity)));
        }
        else
        {
            Display.TooltipSubtitle = FString("Entity #" + eastl::to_string(entt::to_integral(Entity)));
        }

        Display.TooltipChipHeader = "COMPONENTS";
        Display.TooltipChips.clear();
        ECS::Utils::ForEachComponent(Registry, Entity, [&](void*, const entt::basic_sparse_set<>& /*Set*/, entt::meta_type Meta)
        {
            using namespace entt::literals;
            FString Chip = LE_ICON_PUZZLE " ";
            if (entt::meta_any Resolved = ECS::Utils::InvokeMetaFunc(Meta, "static_struct"_hs))
            {
                if (CStruct* StructType = Resolved.cast<CStruct*>())
                {
                    Chip += StructType->MakeDisplayName().c_str();
                    Display.TooltipChips.emplace_back(eastl::move(Chip));
                    return;
                }
            }
            Chip += Meta.name();
            Display.TooltipChips.emplace_back(eastl::move(Chip));
        });
        if (Display.TooltipChips.empty())
        {
            Display.TooltipChips.emplace_back("(none)");
        }

        Display.bTooltipBuilt = true;
    }

    void FSceneEditorTool::RemoveEntityFromOutliner(entt::entity Entity)
    {
        auto It = EntityToTreeNode.find(Entity);
        if (It == EntityToTreeNode.end())
        {
            return;
        }

        // RemoveNode tears down the subtree; walk hierarchy first to clear EntityToTreeNode for descendants.
        FEntityRegistry& Registry = GetSceneRegistry();
        if (Registry.valid(Entity))
        {
            ECS::Utils::ForEachChild(Registry, Entity, [&](entt::entity Child)
            {
                RemoveEntityFromOutliner(Child);
            });
        }

        OutlinerListView.RemoveNode(It->second);
        EntityToTreeNode.erase(It);
    }

    void FSceneEditorTool::ReparentEntityInOutliner(entt::entity Entity)
    {
        // Remember the old tree parent before the move; it may lose its last child here.
        entt::entity OldParent = entt::null;
        if (auto It = EntityToTreeNode.find(Entity); It != EntityToTreeNode.end())
        {
            FTreeNodeID ParentNode = OutlinerListView.GetParentNode(It->second);
            if (ParentNode.IsValid())
            {
                OldParent = OutlinerListView.Get<FEntityListViewItemData>(ParentNode).Entity;
            }
        }

        // Drop and re-add the row; new parent's lazy children rebuild on next expand.
        RemoveEntityFromOutliner(Entity);
        AddEntityToOutliner(Entity);

        // The old parent's expander is stale if that was its only child.
        RefreshOutlinerExpander(OldParent);
    }

    void FSceneEditorTool::RefreshOutlinerExpander(entt::entity Entity)
    {
        if (Entity == entt::null)
        {
            return;
        }
        auto It = EntityToTreeNode.find(Entity);
        if (It == EntityToTreeNode.end())
        {
            return;
        }

        FEntityRegistry& Registry = GetSceneRegistry();
        const FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(Entity);
        const bool bHasChildren = Rel != nullptr && Rel->Children > 0;
        OutlinerListView.MarkHasLazyChildren(It->second, bHasChildren);
    }

    void FSceneEditorTool::RevealEntityInOutliner(entt::entity Entity)
    {
        if (Entity == entt::null)
        {
            return;
        }

        FEntityRegistry& Registry = GetSceneRegistry();
        if (!Registry.valid(Entity) || !IsOutlinerEntityVisible(Entity))
        {
            return;
        }

        // Newly-spawned rows are queued; without this a select-on-spawn would find no node.
        FlushOutlinerPending();

        // Ancestor chain, entity first. Children are lazy, so a row under a never-expanded parent
        // doesn't exist yet -- each level has to be opened top-down before the next one has a node.
        TVector<entt::entity> Chain;
        for (entt::entity Cursor = Entity; Cursor != entt::null && Chain.size() < 64; )
        {
            Chain.push_back(Cursor);

            const FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(Cursor);
            Cursor = (Rel != nullptr) ? Rel->Parent : entt::null;
            if (Cursor != entt::null && !Registry.valid(Cursor))
            {
                break;
            }
        }

        // Root-down, skipping the entity itself (index 0).
        for (int32 i = (int32)Chain.size() - 1; i >= 1; --i)
        {
            auto It = EntityToTreeNode.find(Chain[i]);
            if (It != EntityToTreeNode.end())
            {
                OutlinerListView.ExpandNode(It->second, OutlinerContext);
            }
        }

        auto It = EntityToTreeNode.find(Entity);
        if (It != EntityToTreeNode.end())
        {
            OutlinerListView.RequestScrollToNode(It->second);
        }
    }

    void FSceneEditorTool::BuildEntityChildren(FTreeListView& Tree, FTreeNodeID Item)
    {
        FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
        FEntityRegistry& Registry = GetSceneRegistry();
        if (!Registry.valid(Data.Entity))
        {
            return;
        }

        // Child entity rows: skip filtered-out ones and ones already present (on_construct race).
        ECS::Utils::ForEachChild(Registry, Data.Entity, [&](entt::entity Child)
        {
            if (!IsOutlinerEntityVisible(Child))
            {
                return;
            }
            if (EntityToTreeNode.find(Child) != EntityToTreeNode.end())
            {
                return;
            }

            AddEntityToOutliner(Child);
        });
    }

    void FSceneEditorTool::OnOutlinerEntityConstructed(entt::registry& Registry, entt::entity Entity)
    {
        if (Registry.any_of<FHideInSceneOutliner>(Entity))
        {
            return;
        }
        // Defer to next flush; FRelationshipComponent may not be set yet.
        PendingOutlinerAdds.push_back(Entity);
    }

    void FSceneEditorTool::OnOutlinerEntityDestroyed(entt::registry& Registry, entt::entity Entity)
    {
        (void)Registry;

        // Remember the parent so its expander arrow can be re-evaluated after the destroy settles:
        // deleting an entity's last child must drop the parent's twisty. Prefer the outliner's own
        // tree parent (always accurate for a materialized row); fall back to the registry relationship
        // for a collapsed parent whose child was never built into a row. The SNameComponent destroy
        // signal fires before the FRelationshipComponent one, so the child's parent link is still intact.
        entt::entity ParentEntity = entt::null;
        if (auto It = EntityToTreeNode.find(Entity); It != EntityToTreeNode.end())
        {
            FTreeNodeID ParentNode = OutlinerListView.GetParentNode(It->second);
            if (ParentNode.IsValid())
            {
                ParentEntity = OutlinerListView.Get<FEntityListViewItemData>(ParentNode).Entity;
            }
        }
        else if (const FRelationshipComponent* Rel = GetSceneRegistry().try_get<FRelationshipComponent>(Entity))
        {
            ParentEntity = Rel->Parent;
        }

        RemoveEntityFromOutliner(Entity);
        PendingOutlinerAdds.erase(eastl::remove(PendingOutlinerAdds.begin(), PendingOutlinerAdds.end(), Entity), PendingOutlinerAdds.end());

        if (ParentEntity != entt::null)
        {
            PendingExpanderRefresh.push_back(ParentEntity);
        }
    }

    void FSceneEditorTool::FlushOutlinerPending()
    {
        // Iterate by index; AddEntityToOutliner could grow the queue.
        for (int32 i = 0; i < static_cast<int32>(PendingOutlinerAdds.size()); ++i)
        {
            AddEntityToOutliner(PendingOutlinerAdds[i]);
        }
        PendingOutlinerAdds.clear();

        // Re-evaluate parents whose last child just left: the destroy has fully settled by now, so the
        // registry child count is current. RefreshOutlinerExpander no-ops for parents that were themselves
        // removed (e.g. deleting a whole subtree), so stale entries are harmless.
        for (entt::entity Parent : PendingExpanderRefresh)
        {
            RefreshOutlinerExpander(Parent);
        }
        PendingExpanderRefresh.clear();
    }

    FTransform FSceneEditorTool::GetNewEntitySpawnTransform() const
    {
        return GetCameraSpawnTransform();
    }

    void FSceneEditorTool::CreateEntity()
    {
        BeginCreationTransaction();
        entt::entity NewEntity = World->ConstructEntity("Entity", GetNewEntitySpawnTransform());
        if (NewEntity != entt::null)
        {
            OnEntityCreatedInScene(NewEntity);
        }
        EndTransaction("New Entity");

        if (NewEntity != entt::null)
        {
            SetSingleSelectedEntity(NewEntity);
            MarkSceneDirty();
        }
    }

    void FSceneEditorTool::CreateEntityWithComponent(const CStruct* Component)
    {
        using namespace entt::literals;

        entt::meta_type MetaType = entt::resolve(entt::hashed_string(Component->GetName().c_str()));

        BeginCreationTransaction();
        entt::entity CreatedEntity = World->ConstructEntity(Component->MakeDisplayName(), GetNewEntitySpawnTransform());
        if (CreatedEntity != entt::null)
        {
            ECS::Utils::InvokeMetaFunc(MetaType, "emplace"_hs, entt::forward_as_meta(GetSceneRegistry()), CreatedEntity, entt::forward_as_meta(entt::meta_any{}));
            OnEntityCreatedInScene(CreatedEntity);
        }
        EndTransaction("New Entity");

        if (CreatedEntity != entt::null)
        {
            SetSingleSelectedEntity(CreatedEntity);
            MarkSceneDirty();
        }
    }

    void FSceneEditorTool::CreatePrimitiveEntity(CStaticMesh* PrimitiveMesh, const char* DisplayName)
    {
        if (PrimitiveMesh == nullptr)
        {
            return;
        }

        BeginCreationTransaction();
        entt::entity CreatedEntity = World->ConstructEntity(DisplayName, GetNewEntitySpawnTransform());
        if (CreatedEntity != entt::null)
        {
            GetSceneRegistry().emplace<SStaticMeshComponent>(CreatedEntity).SetStaticMesh(PrimitiveMesh);
            OnEntityCreatedInScene(CreatedEntity);
        }
        EndTransaction("New Primitive");

        if (CreatedEntity != entt::null)
        {
            SetSingleSelectedEntity(CreatedEntity);
            MarkSceneDirty();
        }
    }

    TVector<entt::entity> FSceneEditorTool::GetComponentEditTargets(entt::entity Entity)
    {
        TVector<entt::entity> Targets;

        FEntityRegistry& Registry = GetSceneRegistry();
        if (!Registry.valid(Entity))
        {
            return Targets;
        }

        // Apply to the whole selection when more than one entity is selected; otherwise just the focus entity.
        if (SelectedEntities.size() > 1 && IsEntitySelected(Entity))
        {
            Targets.reserve(SelectedEntities.size());
            for (entt::entity Selected : SelectedEntities)
            {
                if (Registry.valid(Selected))
                {
                    Targets.push_back(Selected);
                }
            }
        }
        else
        {
            Targets.push_back(Entity);
        }

        return Targets;
    }

    void FSceneEditorTool::ApplyAddComponentToTargets(const TVector<entt::entity>& Targets, entt::meta_type PickedMetaType)
    {
        using namespace entt::literals;

        if (Targets.empty())
        {
            return;
        }

        FEntityRegistry& Registry = GetSceneRegistry();

        // Resolve the reflected CStruct so an instance can record the add in its override ledger.
        CStruct* AddedStruct = nullptr;
        if (PickedMetaType)
        {
            if (entt::meta_any S = ECS::Utils::InvokeMetaFunc(PickedMetaType, "static_struct"_hs))
            {
                AddedStruct = S.cast<CStruct*>();
            }
        }

        // The reflected "emplace" uses emplace_or_replace for data components (would reset existing
        // data) and plain emplace for tags (would assert if present), so targets that already hold the
        // component are separated out rather than emplaced over.
        //
        // Partitioned before the transaction opens: an add that turns out to be a no-op should not
        // leave an empty step on the undo stack for the user to step back through.
        TVector<entt::entity> Missing;
        Missing.reserve(Targets.size());

        uint32 AlreadyPresent = 0;

        for (entt::entity Target : Targets)
        {
            if (ECS::Utils::HasComponent(Registry, Target, PickedMetaType))
            {
                ++AlreadyPresent;
            }
            else
            {
                Missing.push_back(Target);
            }
        }

        if (Missing.empty())
        {
            // Nothing happened, which without saying so is indistinguishable from the click not
            // registering.
            if (Targets.size() == 1)
            {
                ImGuiX::Notifications::NotifyWarning("This entity already has {0}.", AddedStruct ? AddedStruct->GetName().c_str() : "that component");
            }
            else
            {
                ImGuiX::Notifications::NotifyWarning("All {0} selected entities already have {1}.",
                    (uint32)Targets.size(), AddedStruct ? AddedStruct->GetName().c_str() : "that component");
            }

            return;
        }

        BeginTransaction();
        for (entt::entity Target : Missing)
        {
            ECS::Utils::InvokeMetaFunc(PickedMetaType, "emplace"_hs, entt::forward_as_meta(Registry), Target, entt::forward_as_meta(entt::meta_any{}));
            if (AddedStruct != nullptr)
            {
                CPrefab::NoteComponentAdded(Registry, Target, AddedStruct);
            }
        }
        EndTransaction("Add Component");

        if (AlreadyPresent > 0)
        {
            ImGuiX::Notifications::NotifyInfo("Added {0} to {1} entities; {2} already had it.",
                AddedStruct ? AddedStruct->GetName().c_str() : "component", (uint32)Missing.size(), AlreadyPresent);
        }

        MarkSceneDirty();
        OutlinerListView.MarkTreeDirty();
        bDetailsDirty = true;
    }

    namespace
    {
        // Shared visuals for the Add / Create-Entity picker. Kept compact and purely vertical: a
        // collapsing section header per group, then a tight 2-column table (icon, name) of full-width
        // selectable rows. Scales to large component counts without the old card-button style.
        constexpr ImGuiTableFlags GPickerTableFlags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_PadOuterX;

        // Maps a component Category to a representative icon; falls back to a generic component glyph.
        const char* PickerCategoryIcon(const FString& Name)
        {
            if (Name == "Rendering")   return LE_ICON_CUBE_OUTLINE;
            if (Name == "Lights")      return LE_ICON_LIGHTBULB;
            if (Name == "Environment") return LE_ICON_WEATHER_PARTLY_CLOUDY;
            if (Name == "Physics")     return LE_ICON_ATOM;
            if (Name == "Audio")       return LE_ICON_VOLUME_HIGH;
            if (Name == "AI")          return LE_ICON_ROBOT;
            if (Name == "Camera")      return LE_ICON_CAMERA;
            if (Name == "Animation")   return LE_ICON_RUN;
            if (Name == "Gameplay")    return LE_ICON_GAMEPAD_VARIANT;
            if (Name == "Networking")  return LE_ICON_LAN;
            if (Name == "Character")   return LE_ICON_ACCOUNT;
            if (Name == "Movement")    return LE_ICON_WALK;
            if (Name == "Effects")     return LE_ICON_FIRE;
            if (Name == "Foliage")     return LE_ICON_GRASS;
            if (Name == "Terrain")     return LE_ICON_TERRAIN;
            if (Name == "Destruction") return LE_ICON_HAMMER_WRENCH;
            if (Name == "UI")          return LE_ICON_VIEW_DASHBOARD;
            if (Name == "Prefab")      return LE_ICON_PACKAGE_VARIANT_CLOSED;
            return LE_ICON_PUZZLE_OUTLINE;
        }

        // Collapsing group header with a trailing count. Force-opens while a search filter is active so
        // matches stay visible even if the user previously collapsed the section. Returns whether open.
        bool BeginPickerSection(const char* Icon, const char* Label, int32 Count, bool bForceOpen)
        {
            if (bForceOpen)
            {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            }

            FFixedString Header;
            Header.append(Icon);
            Header.append("  ");
            Header.append(Label);
            Header.append("  (");
            Header.append(eastl::to_string(Count).c_str());
            Header.append(")");

            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::SectionHeader());
            const bool bOpen = ImGui::CollapsingHeader(Header.c_str(),
                ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
            ImGui::PopStyleColor();
            return bOpen;
        }

        // One compact picker row: [icon] Label with a full-row hover/click target. Returns true on click.
        // Expects to be called between BeginTable/EndTable on a 2-column (icon, name) table.
        bool DrawPickerRow(const void* ID, const char* Icon, const char* Label)
        {
            ImGui::PushID(ID);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            const bool bClicked = ImGui::Selectable("##sel", false,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);

            ImGui::SameLine();
            // Soft slate tone (matches the section header) keeps the icon column a subtle visual rail
            // rather than a loud accent stripe down a long list.
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::SectionHeader());
            ImGui::TextUnformatted(Icon);
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(Label);

            ImGui::PopID();
            return bClicked;
        }
    }

    bool FSceneEditorTool::DrawAddableComponentList(const ImGuiTextFilter& Filter, const TVector<entt::entity>& Targets, entt::meta_type& OutMetaType, CStruct*& OutStruct)
    {
        struct FComponentEntry
        {
            entt::meta_type MetaType;
            CStruct*        Struct = nullptr;   // reflected component
        };

        struct FComponentCategory
        {
            FString                  Name;
            TVector<FComponentEntry> Entries;
        };

        TVector<FComponentCategory> Categories;
        auto FindOrAddCategory = [&Categories](const FString& Name) -> FComponentCategory&
        {
            for (FComponentCategory& Cat : Categories)
            {
                if (Cat.Name == Name)
                {
                    return Cat;
                }
            }
            FComponentCategory& Added = Categories.emplace_back();
            Added.Name = Name;
            return Added;
        };

        static const FString DefaultCategoryName = "General";

        for(auto &&[ID, MetaType]: entt::resolve())
        {
            ECS::ETraits Traits = MetaType.traits<ECS::ETraits>();
            if (!EnumHasAllFlags(Traits, ECS::ETraits::Component))
            {
                continue;
            }

            using namespace entt::literals;
            entt::meta_any Any = ECS::Utils::InvokeMetaFunc(MetaType, "static_struct"_hs);
            CStruct* Struct = Any.cast<CStruct*>();
            ASSERT(Struct);

            if (Struct->HasMeta("HideInComponentList"))
            {
                continue;
            }

            FFixedString DisplayName = Struct->MakeDisplayName();
            if (!Filter.PassFilter(DisplayName.c_str()))
            {
                continue;
            }

            FString CategoryName = Struct->HasMeta("Category")
                ? Struct->GetMeta("Category")
                : DefaultCategoryName;

            FComponentEntry NewEntry;
            NewEntry.MetaType = MetaType;
            NewEntry.Struct   = Struct;
            FindOrAddCategory(CategoryName).Entries.push_back(NewEntry);
        }

        eastl::sort(Categories.begin(), Categories.end(), [](const FComponentCategory& LHS, const FComponentCategory& RHS)
        {
            // Push "General" to the bottom so categorized buckets surface first.
            const bool bLhsGeneral = (LHS.Name == DefaultCategoryName);
            const bool bRhsGeneral = (RHS.Name == DefaultCategoryName);
            if (bLhsGeneral != bRhsGeneral)
            {
                return !bLhsGeneral;
            }
            return LHS.Name < RHS.Name;
        });

        bool bPicked = false;
        const bool bFiltering = Filter.IsActive();

        FEntityRegistry& Registry = GetSceneRegistry();

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 3.0f));
        for (FComponentCategory& Category : Categories)
        {
            auto EntryName = [](const FComponentEntry& E) -> FString
            {
                return E.Struct->GetName().ToString();
            };
            eastl::sort(Category.Entries.begin(), Category.Entries.end(), [&](const FComponentEntry& LHS, const FComponentEntry& RHS)
            {
                return EntryName(LHS) < EntryName(RHS);
            });

            ImGui::PushID(Category.Name.c_str());

            const char* CategoryIcon = PickerCategoryIcon(Category.Name);
            if (BeginPickerSection(CategoryIcon, Category.Name.c_str(), (int32)Category.Entries.size(), bFiltering))
            {
                if (ImGui::BeginTable("##Components", 2, GPickerTableFlags))
                {
                    ImGui::TableSetupColumn("##Icon", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                    ImGui::TableSetupColumn("##Name", ImGuiTableColumnFlags_WidthStretch);

                    for (const FComponentEntry& Entry : Category.Entries)
                    {
                        // Offering a component every target already holds is offering a no-op: the add
                        // skips them rather than replacing, so the click would do nothing at all. Stops
                        // at the first target that lacks it, which is the usual answer on the first one.
                        bool bEveryTargetHasIt = !Targets.empty();
                        for (entt::entity Target : Targets)
                        {
                            if (!ECS::Utils::HasComponent(Registry, Target, Entry.MetaType))
                            {
                                bEveryTargetHasIt = false;
                                break;
                            }
                        }

                        ImGui::BeginDisabled(bEveryTargetHasIt);

                        // Stable per-item ID across frames (the entry list is rebuilt every frame, so its
                        // address is not stable -- an unstable ID breaks click press/release matching).
                        FFixedString DisplayName = Entry.Struct->MakeDisplayName();
                        if (DrawPickerRow((void*)Entry.Struct, CategoryIcon, DisplayName.c_str()))
                        {
                            OutMetaType = Entry.MetaType;
                            OutStruct   = Entry.Struct;
                            bPicked = true;
                        }

                        ImGui::EndDisabled();

                        // Tooltip outside the disabled scope: a disabled item does not report hover.
                        if (bEveryTargetHasIt && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            ImGui::SetTooltip(Targets.size() == 1
                                ? "This entity already has this component."
                                : "Every selected entity already has this component.");
                        }
                    }

                    ImGui::EndTable();
                }
            }

            ImGui::PopID();
        }
        ImGui::PopStyleVar();

        return bPicked;
    }

    void FSceneEditorTool::AddEntityToCopies(entt::entity Entity)
    {
        GetSceneRegistry().emplace_or_replace<FCopiedTag>(Entity);
    }

    void FSceneEditorTool::RemoveEntityFromCopies(entt::entity Entity)
    {
        GetSceneRegistry().remove<FCopiedTag>(Entity);
    }

    void FSceneEditorTool::ClearCopies() const
    {
        GetSceneRegistry().clear<FCopiedTag>();
    }

    void FSceneEditorTool::CopyEntity(entt::entity& To, entt::entity From)
    {
        World->DuplicateEntity(To, From, &EditorEntityUtils::DefaultDuplicateFilter);
    }

    void FSceneEditorTool::CycleGuizmoOp()
    {
        EditorEntityUtils::CycleGizmoOp(GuizmoOp);
    }

    void FSceneEditorTool::DrawViewportToolbar(const FUpdateContext& UpdateContext)
    {
        if (FInputViewportRegistry::Get().IsGameInputFocused())
        {
            return;
        }

        float ButtonSize = 0.0f;
        if (BeginViewportToolbarWindow(ButtonSize))
        {
            ImGui::BeginGroup();

            // Leading play/simulate controls (world only); draws its own trailing separator.
            DrawViewportToolbarPlayControls(ButtonSize);

            // The rest of the bar is shown whenever the editor is focused, even mid-play, so debug/view
            // controls stay reachable (the game-focused case returned early above).
            DrawCameraControls(ButtonSize);

            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();

            DrawViewportOptions(ButtonSize);

            // Trailing editor-mode selector + active-mode toolbar (world only).
            if (!IsViewportPlaying())
            {
                DrawViewportToolbarModeSelector(ButtonSize);
            }

            ImGui::EndGroup();
        }
        EndViewportToolbarWindow();
    }

    void FSceneEditorTool::DrawCameraControls(float ButtonSize)
    {
        const ImVec2 BtnSize = ImVec2(ButtonSize, ButtonSize);
        float Speed = CameraState.Speed;

        if (ImGuiX::IconButton(LE_ICON_CAMERA, "##Camera", 0xFFFFFFFF, BtnSize))
        {
            ImGui::OpenPopup("CameraSettings");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Camera Speed: %.1fx", Speed);
        }

        if (ImGui::BeginPopup("CameraSettings", ImGuiWindowFlags_NoMove))
        {
            // The editor camera (EditorEntity) lives in the tool's own World, not the observed world.
            STransformComponent& CameraTransform = World->GetComponent<STransformComponent>(EditorEntity);

            ImGui::SeparatorText(LE_ICON_VIDEO " Camera Settings");

            bool bHasCameraLight = World->HasComponent<SPointLightComponent>(EditorEntity);
            ImGui::TextUnformatted("Camera Light");
            if (ImGuiX::IconButton(LE_ICON_LIGHTBULB, "Light"))
            {
                if (bHasCameraLight)
                {
                    World->RemoveComponent<SPointLightComponent>(EditorEntity);
                    bHasCameraLight = false;
                }
                else
                {
                    auto&& PL = World->EmplaceComponent<SPointLightComponent>(EditorEntity);
                    PL.Intensity = 50.0f;
                    PL.Attenuation = 50.0f;
                }
            }
            
            if (bHasCameraLight)
            {
                auto&& PL = World->GetComponent<SPointLightComponent>(EditorEntity);
                ImGuiX::SliderFloat("Intensity", &PL.Intensity, 0.0f, 300.0f);
                ImGuiX::SliderFloat("Attenuation", &PL.Attenuation, 0.0f, 200.0f);
                ImGui::ColorEdit3("Color", &PL.LightColor.x, ImGuiColorEditFlags_Float);
            }
            
            ImGui::Separator();
            
            ImGui::TextUnformatted("Movement Speed");
            if (ImGui::SliderFloat("##Speed", &Speed, 0.1f, 100.0f, "%.1fx"))
            {
                CameraState.Speed = Speed;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##Speed"))
            {
                Speed = 1.0f;
                CameraState.Speed = 1.0f;
            }

            ImGui::Separator();

            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Accent());
            ImGui::TextUnformatted(LE_ICON_AXIS_ARROW);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGuiX::TextTooltip("Translation (Location)");
            FVector3 CameraLocation = CameraTransform.GetWorldLocation();
            if (ImGui::DragFloat3("T", Math::ValuePtr(CameraLocation), 0.01f))
            {
                CameraTransform.SetLocalLocation(CameraLocation);
            }

            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Success());
            ImGui::TextUnformatted(LE_ICON_ROTATE_360);
            ImGui::PopStyleColor();
            ImGuiX::TextTooltip("Rotation (Euler Angles)");
            ImGui::SameLine();
            FVector3 EulerRotation = CameraTransform.GetRotationAsEuler();
            if (ImGui::DragFloat3("R", Math::ValuePtr(EulerRotation), 0.01f))
            {
                CameraTransform.SetRotationFromEuler(EulerRotation);
            }

            ImGui::Separator();
            if (ImGui::Button("Reset Position", ImVec2(-1, 0)))
            {
                World->GetComponent<STransformComponent>(EditorEntity).SetLocation(FVector3(0.0f));
            }
            if (ImGui::Button("Reset Rotation", ImVec2(-1, 0)))
            {
                World->GetComponent<STransformComponent>(EditorEntity).SetRotation(FQuat(1.0f, 0.0f, 0.0f, 0.0f));
            }
            ImGui::Spacing();
            if (ImGui::Button("Close", ImVec2(-1, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        if (ImGuiX::IconButton(LE_ICON_CROSSHAIRS, "##FocusSelection", 0xFFFFFFFF, BtnSize))
        {
            FocusViewportToEntity(GetLastSelectedEntity());
        }
        ImGuiX::TextTooltip("Focus on Selection (F)");
    }

    void FSceneEditorTool::DrawSnapSettingsPopup()
    {
        ImGui::Text("Snap Settings");
        ImGuiX::HelpMarker(
            "Constrains gizmo drags to fixed steps. Translate = world units. Rotate = degrees. "
            "Scale = multiplicative factor. Toggle quickly with the Snap button on the toolbar.");
        ImGui::Separator();

        if (ImGui::Checkbox("Enable Snap", &bGuizmoSnapEnabled))
        {
            PersistGizmoSettings();
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Header, EditorColors::WithAlpha(EditorColors::Accent(), 0.3f));
        bool bAnySettingDirty = false;

        if (ImGui::CollapsingHeader("Translation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PushID("Translate");
            ImGui::Indent();
            ImGui::BeginDisabled(!bGuizmoSnapEnabled);
            ImGui::Text("Presets:"); ImGui::SameLine();
            if (ImGui::Button("0.1")) { GuizmoSnapTranslate = 0.1f; bAnySettingDirty = true; } ImGui::SameLine();
            if (ImGui::Button("1.0")) { GuizmoSnapTranslate = 1.0f; bAnySettingDirty = true; } ImGui::SameLine();
            if (ImGui::Button("5.0")) { GuizmoSnapTranslate = 5.0f; bAnySettingDirty = true; } ImGui::SameLine();
            if (ImGui::Button("10"))  { GuizmoSnapTranslate = 10.0f; bAnySettingDirty = true; } ImGui::SameLine();
            if (ImGui::Button("50"))  { GuizmoSnapTranslate = 50.0f; bAnySettingDirty = true; }
            if (ImGui::DragFloat("Value##Translation", &GuizmoSnapTranslate, 0.1f, 0.01f, 1000.0f, "%.2f units")) { bAnySettingDirty = true; }
            ImGui::EndDisabled();
            ImGui::Unindent();
            ImGui::PopID();
        }

        if (ImGui::CollapsingHeader("Rotation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PushID("Rotate");
            ImGui::Indent();
            ImGui::BeginDisabled(!bGuizmoSnapEnabled);
            ImGui::Text("Presets:"); ImGui::SameLine();
            if (ImGui::Button("1 " LE_ICON_ANGLE_ACUTE))  { GuizmoSnapRotate = 1.0f; bAnySettingDirty = true; } ImGui::SameLine();
            if (ImGui::Button("5 " LE_ICON_ANGLE_ACUTE))  { GuizmoSnapRotate = 5.0f; bAnySettingDirty = true; } ImGui::SameLine();
            if (ImGui::Button("15 " LE_ICON_ANGLE_ACUTE)) { GuizmoSnapRotate = 15.0f; bAnySettingDirty = true; } ImGui::SameLine();
            if (ImGui::Button("45 " LE_ICON_ANGLE_ACUTE)) { GuizmoSnapRotate = 45.0f; bAnySettingDirty = true; } ImGui::SameLine();
            if (ImGui::Button("90 " LE_ICON_ANGLE_ACUTE)) { GuizmoSnapRotate = 90.0f; bAnySettingDirty = true; }
            if (ImGui::DragFloat("Value##Rotation", &GuizmoSnapRotate, 0.5f, 0.1f, 180.0f, "%.1f " LE_ICON_ANGLE_ACUTE)) { bAnySettingDirty = true; }
            ImGui::EndDisabled();
            ImGui::Unindent();
            ImGui::PopID();
        }

        if (ImGui::CollapsingHeader("Scale", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PushID("Scale");
            ImGui::Indent();
            ImGui::BeginDisabled(!bGuizmoSnapEnabled);
            ImGui::Text("Presets:"); ImGui::SameLine();
            if (ImGui::Button("0.1"))  { GuizmoSnapScale = 0.1f; bAnySettingDirty = true; } ImGui::SameLine();
            if (ImGui::Button("0.25")) { GuizmoSnapScale = 0.25f; bAnySettingDirty = true; } ImGui::SameLine();
            if (ImGui::Button("0.5"))  { GuizmoSnapScale = 0.5f; bAnySettingDirty = true; } ImGui::SameLine();
            if (ImGui::Button("1.0"))  { GuizmoSnapScale = 1.0f; bAnySettingDirty = true; }
            if (ImGui::DragFloat("Value##Scale", &GuizmoSnapScale, 0.01f, 0.01f, 10.0f, "%.2f")) { bAnySettingDirty = true; }
            ImGui::EndDisabled();
            ImGui::Unindent();
            ImGui::PopID();
        }

        if (bAnySettingDirty)
        {
            PersistGizmoSettings();
        }

        ImGui::PopStyleColor();
    }

    void FSceneEditorTool::DrawViewportOptions(float ButtonSize)
    {
        const ImVec2 BtnSize = ImVec2(ButtonSize, ButtonSize);

        ImColor IconColor = bWorldGridEnabled ? ImColor(EditorColors::Accent()) : ImColor(EditorColors::TextDim());
        if (ImGuiX::IconButton(LE_ICON_GRID, "##GridToggle", IconColor, BtnSize))
        {
            bWorldGridEnabled = !bWorldGridEnabled;
        }
        ImGuiX::TextTooltip("Toggle Grid");

        ImGui::SameLine();

        const char* Icon = nullptr;
        switch (GuizmoOp)
        {
        case ImGuizmo::OPERATION::TRANSLATE: Icon = LE_ICON_AXIS_ARROW; break;
        case ImGuizmo::OPERATION::ROTATE:    Icon = LE_ICON_ROTATE_360; break;
        case ImGuizmo::OPERATION::SCALE:     Icon = LE_ICON_ARROW_TOP_RIGHT_BOTTOM_LEFT; break;
        }

        if (ImGuiX::IconButton(Icon, "##GizmoMode", 0xFFFFFFFF, BtnSize))
        {
            CycleGuizmoOp();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Gizmo: %s (R)", ImGuiX::ImGuizmoOpToString(GuizmoOp).data());
        }

        ImGui::SameLine();

        const bool bIsLocalMode = (GuizmoMode == ImGuizmo::LOCAL);
        const char* ModeIcon = bIsLocalMode ? LE_ICON_AXIS_ARROW : LE_ICON_EARTH;
        const ImColor ModeIconColor = bIsLocalMode ? ImColor(EditorColors::Accent()) : ImColor(EditorColors::TextPrimary());
        if (ImGuiX::IconButton(ModeIcon, "##GizmoSpace", ModeIconColor, BtnSize))
        {
            ToggleGuizmoMode();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Gizmo Space: %s (X)", bIsLocalMode ? "Local" : "World");
        }

        if (bGuizmoSnapEnabled)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, EditorColors::WithAlpha(EditorColors::Accent(), 0.6f));
        }
        ImGui::SameLine();
        bool bSnapWasEnabled = bGuizmoSnapEnabled;
        if (ImGuiX::IconButton(LE_ICON_MAGNET, "##SnapToggle", 0xFFFFFFFF, BtnSize))
        {
            bGuizmoSnapEnabled = !bGuizmoSnapEnabled;
            PersistGizmoSettings();
        }
        if (bSnapWasEnabled)
        {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Snap Settings (Click to toggle) (Right click for config)");
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
        {
            ImGui::OpenPopup("SnapSettingsPopup");
        }
        if (ImGui::BeginPopup("SnapSettingsPopup", ImGuiWindowFlags_NoMove))
        {
            DrawSnapSettingsPopup();
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        // Shared with every world-owning tool; see FEditorTool::DrawViewModeButton.
        DrawViewModeButton(ButtonSize);

        ImGui::SameLine();
        if (ImGuiX::IconButton(LE_ICON_PLUS, "##AddToWorld", 0xFFFFFFFF, BtnSize))
        {
            ImGui::OpenPopup("AddToEntityMenu");
        }
        DrawAddToEntityOrWorldPopup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Add something to the scene.");
        }

    }

    void FSceneEditorTool::ToggleGuizmoMode()
    {
        EditorEntityUtils::ToggleGizmoMode(GuizmoMode);
    }

    void FSceneEditorTool::UpdateCameraPreview()
    {
        bCameraPreviewActive = false;

        IRenderScene* RenderScene = World ? World->GetRenderer() : nullptr;
        if (RenderScene == nullptr)
        {
            return;
        }

        // The render scene can be torn down + rebuilt (idle reclaim), invalidating our handle. The
        // pointer compare catches most rebuilds, but a new scene can land at the freed scene's
        // address, so SetCaptureView's return below is the authoritative staleness check.
        if (RenderScene != CameraPreviewScene)
        {
            CameraPreviewScene  = RenderScene;
            CameraPreviewHandle = -1;
        }

        // Only preview a selected camera entity in the tool's own world (the capture renders
        // World's scene, so a foreign observed world's selection has nothing to render), and
        // never inside a running game world. Otherwise leave the capture registered but
        // disabled (no render cost).
        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        const entt::entity Selected = GetLastSelectedEntity();
        const bool bWantPreview =
            AllowCameraPreview() &&
            !World->IsGameWorld() && !IsInspectingForeignWorld() &&
            Registry.valid(Selected) &&
            Registry.all_of<SCameraComponent, STransformComponent>(Selected);

        if (!bWantPreview)
        {
            if (CameraPreviewHandle >= 0 && !RenderScene->SetCaptureView(CameraPreviewHandle, FViewVolume{}, false))
            {
                CameraPreviewHandle = -1;   // stale handle from a rebuilt scene
            }
            return;
        }

        // Build the camera's view from its world transform + FOV (its own ViewVolume isn't
        // resolved while it's a non-active camera). Forward/up convention matches SCameraSystem.
        const SCameraComponent& Camera = Registry.get<SCameraComponent>(Selected);
        STransformComponent& Transform = Registry.get<STransformComponent>(Selected);
        (void)Transform.GetWorldMatrix();   // ensure the world transform is current

        const FVector3 Position = Transform.GetWorldLocation();
        const FQuat Rotation = Transform.GetWorldRotation();
        const FVector3 Forward  = Rotation * FVector3(0.0f, 0.0f, 1.0f);
        const FVector3 Up       = Rotation * FVector3(0.0f, 1.0f, 0.0f);

        // Use the authored FOV property, not GetFOV(): ViewVolume only tracks the property for
        // the active camera, so a non-active selected camera's value would be stale.
        FViewVolume View(Camera.FOV, (float)CameraPreviewWidth / (float)CameraPreviewHeight);
        View.SetView(Position, Forward, Up);

        // Two attempts: if the first SetCaptureView rejects the handle (scene rebuilt at the same
        // address, so the pointer compare above missed it), register against the live scene and retry.
        for (int32 Attempt = 0; Attempt < 2; ++Attempt)
        {
            if (CameraPreviewHandle < 0)
            {
                CameraPreviewHandle = RenderScene->RegisterCaptureView(FUIntVector2(CameraPreviewWidth, CameraPreviewHeight));
                if (CameraPreviewHandle < 0)
                {
                    return;
                }
            }

            if (RenderScene->SetCaptureView(CameraPreviewHandle, View, true))
            {
                bCameraPreviewActive = true;
                return;
            }

            CameraPreviewHandle = -1;
        }
    }

    void FSceneEditorTool::DrawCameraPreviewOverlay(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize)
    {
        // Selected-camera PiP, pinned bottom-right. The render scene shades it into a capture
        // view (in UpdateCameraPreview); here we just composite it by its heap ResourceID.
        // Drag the top-left corner grip to resize; the scale persists per tool.
        bCameraPreviewMouseOver = false;
        const int32 PreviewID = (bCameraPreviewActive && CameraPreviewHandle >= 0 && World->GetRenderer() != nullptr)
            ? World->GetRenderer()->GetCaptureDisplayResourceID(CameraPreviewHandle) : -1;
        if (PreviewID < 0)
        {
            bCameraPreviewResizing = false;
            return;
        }

        const float Margin = 14.0f;
        const ImVec2 Max(ViewportOrigin.x + ViewportSize.x - Margin,
                         ViewportOrigin.y + ViewportSize.y - Margin);

        const ImVec2 Mouse = ImGui::GetMousePos();
        if (bCameraPreviewResizing)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                // Bottom-right corner is the anchor; scale follows the dragged top-left corner.
                const float ScaleX = (Max.x - Mouse.x) / (float)CameraPreviewWidth;
                const float ScaleY = (Max.y - Mouse.y) / (float)CameraPreviewHeight;
                CameraPreviewScale = Math::Clamp(Math::Max(ScaleX, ScaleY), 0.25f, 1.5f);
            }
            else
            {
                bCameraPreviewResizing = false;
                PersistCameraPreviewScale();
            }
        }

        // Fit-clamp keeps the preview inside small viewports regardless of the saved scale.
        const float FitScale = Math::Min((ViewportSize.x * 0.85f) / (float)CameraPreviewWidth,
                                         (ViewportSize.y * 0.85f) / (float)CameraPreviewHeight);
        const float Scale = Math::Min(CameraPreviewScale, Math::Max(FitScale, 0.1f));
        const ImVec2 Size((float)CameraPreviewWidth * Scale, (float)CameraPreviewHeight * Scale);
        const ImVec2 Min(Max.x - Size.x, Max.y - Size.y);

        const float GripExtent = 14.0f;
        const bool bOverGrip = Mouse.x >= Min.x - 6.0f && Mouse.x <= Min.x + GripExtent &&
                               Mouse.y >= Min.y - 6.0f && Mouse.y <= Min.y + GripExtent;
        if (!bCameraPreviewResizing && bOverGrip &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            bCameraPreviewResizing = true;
        }
        bCameraPreviewMouseOver = bOverGrip || bCameraPreviewResizing;
        if (bCameraPreviewMouseOver)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        }

        ImDrawList* DL = ImGui::GetWindowDrawList();
        DL->AddRectFilled(ImVec2(Min.x - 3.0f, Min.y - 18.0f), ImVec2(Max.x + 3.0f, Max.y + 3.0f),
            IM_COL32(0, 0, 0, 190), 4.0f);
        DL->AddText(ImVec2(Min.x + 2.0f, Min.y - 16.0f), IM_COL32(235, 235, 235, 220), "Camera Preview");
        DL->AddImage(ImGuiX::ToImTextureRef((uint32)PreviewID), Min, Max);
        DL->AddRect(Min, Max, IM_COL32(255, 255, 255, 110), 2.0f);

        // Corner resize grip: diagonal ticks, brightened while hovered/dragging.
        const ImU32 GripCol = bCameraPreviewMouseOver ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 140);
        DL->AddLine(ImVec2(Min.x + 2.0f, Min.y + 9.0f),  ImVec2(Min.x + 9.0f,  Min.y + 2.0f), GripCol, 2.0f);
        DL->AddLine(ImVec2(Min.x + 2.0f, Min.y + 14.0f), ImVec2(Min.x + 14.0f, Min.y + 2.0f), GripCol, 2.0f);
    }

    void FSceneEditorTool::EndFrame()
    {
        using namespace entt::literals;

        if (!bShowComponentVisualizers)
        {
            return;
        }

        CComponentVisualizerRegistry& ComponentVisualizerRegistry = CComponentVisualizerRegistry::Get();
        FEntityRegistry& Registry = GetSceneRegistry();

        // Resolve which component storages actually have a visualizer ONCE per frame.
        TFixedVector<eastl::pair<entt::sparse_set*, CComponentVisualizer*>, 16> VisualizerStorages;
        for (auto&& [ID, Storage] : Registry.storage())
        {
            if (entt::meta_type MetaType = entt::resolve(Storage.info()))
            {
                if (entt::meta_any ReturnValue = ECS::Utils::InvokeMetaFunc(MetaType, "static_struct"_hs))
                {
                    if (CComponentVisualizer* Visualizer = ComponentVisualizerRegistry.GetComponentVisualizer(ReturnValue.cast<CStruct*>()))
                    {
                        VisualizerStorages.emplace_back(&Storage, Visualizer);
                    }
                }
            }
        }

        if (VisualizerStorages.empty())
        {
            return;
        }

        // Flatten the selection (view, not SelectedEntities, so entt::exclude<SDisabledTag> applies) into a
        // contiguous list to index from worker threads. Bail before the resolve when nothing is selected.
        TVector<entt::entity> SelectedEntities;
        auto SelectedView = Registry.view<FSelectedInEditorComponent>(entt::exclude<SDisabledTag>);
        SelectedEntities.reserve(SelectedView.size_hint());
        SelectedView.each([&](entt::entity SelectedEntity)
        {
            SelectedEntities.push_back(SelectedEntity);
        });

        if (SelectedEntities.empty())
        {
            return;
        }
        
        ECS::Utils::ResolveAllDirtyTransforms(Registry);

        auto DrawFor = [&](entt::entity Entity)
        {
            for (auto& [Storage, Visualizer] : VisualizerStorages)
            {
                if (Storage->contains(Entity))
                {
                    Visualizer->Draw(World, Registry, Entity);
                }
            }
        };
        
        Task::ParallelFor((uint32)SelectedEntities.size(), [&](uint32 Index)
        {
            const entt::entity SelectedEntity = SelectedEntities[Index];
            DrawFor(SelectedEntity);
            ECS::Utils::ForEachChild(Registry, SelectedEntity, [&](entt::entity Child)
            {
                DrawFor(Child);
            });
        }, 32);
    }

    void FSceneEditorTool::DrawDetailsPanel(bool bFocused)
    {
        const entt::entity Entity = GetLastSelectedEntity();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorColors::PanelBg());
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);

        ImGui::BeginChild("Property Editor", ImVec2(0, 0), true);

        // PropertyTables hold raw component pointers; rebuild before drawing on focus change, invalidation, or dirty mark.
        const bool bEntityValid = (Entity != entt::null) && GetSceneRegistry().valid(Entity);

        // A C# reload rebuilds every script component's value buffer and destroys the CScriptStruct that
        // describes it, so a table built against the previous generation points at freed memory in both
        // its data and its layout. Nothing broadcasts that, so the generation is compared here rather
        // than trusting an invalidation that no one sends.
        // A prefab refresh is the same hazard: it removes and re-emplaces components (entt's swap-and-pop
        // relocates a pool's last element into the freed slot, so even an untouched neighbour's row can end
        // up aliasing someone else's component) and a re-capture replaces the whole prefab registry the
        // reset-to-prefab baselines point into.
        const int32  ScriptGeneration = DotNet::GetScriptGeneration();
        const uint32 PrefabGeneration = CPrefab::GetDataGeneration();
        if (ScriptGeneration != DetailsScriptGeneration || PrefabGeneration != DetailsPrefabGeneration)
        {
            DetailsScriptGeneration = ScriptGeneration;
            DetailsPrefabGeneration = PrefabGeneration;

            // Dropped rather than rebuilt: the old tables must not be touched again, and the rebuild
            // below only runs when there is a valid entity to rebuild for.
            PropertyTables.clear();
            DetailsEntity = entt::null;
            bDetailsDirty = true;
        }

        if (!bEntityValid)
        {
            if (DetailsEntity != entt::null || !PropertyTables.empty())
            {
                PropertyTables.clear();
                DetailsEntity = entt::null;
            }
            bDetailsDirty = false;
        }
        else if (DetailsEntity != Entity || bDetailsDirty)
        {
            RebuildPropertyTables(Entity);
            DetailsEntity = Entity;
            bDetailsDirty = false;
        }

        if (bEntityValid)
        {
            DrawEntityProperties(Entity);
        }
        else
        {
            DrawEmptyState();
        }

        ImGui::EndChild();

        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // Lives on the base rather than the world tool: the details header offers Rename for every
    // scene-flavored tool, and the prefab editor edits a preview world that GetSceneRegistry
    // resolves and ECS::GetWorldRegistry(*World) would not.
    void FSceneEditorTool::PushRenameEntityModal(entt::entity Entity)
    {
        ToolContext->PushModal("Rename Entity", ImVec2(450.0f, 250.0f), [this, Entity]() -> bool
        {
            auto& NameComponent = GetSceneRegistry().get<SNameComponent>(Entity);
            static FFixedString InputBuffer;
    
            if (ImGui::IsWindowAppearing())
            {
                InputBuffer = NameComponent.Name.c_str();
            }
    
            ImGui::Text("Enter new name:");
            ImGui::Spacing();
    
            ImGui::SetNextItemWidth(-1.0f);
            bool bShouldClose = ImGui::InputText("##Name", InputBuffer.data(), 
                                                  InputBuffer.max_size(), 
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            constexpr float ButtonWidth = 100.0f;
            const float AvailWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX((AvailWidth - ButtonWidth * 2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    
            if (ImGui::Button("OK", ImVec2(ButtonWidth, 0.0f)) || bShouldClose)
            {
                NameComponent.Name = FName(InputBuffer.c_str());

                // Update just this entity's row label rather than rebuilding the whole tree.
                auto It = EntityToTreeNode.find(Entity);
                if (It != EntityToTreeNode.end())
                {
                    FFixedString Label;
                    Label.append(LE_ICON_CUBE).append(" ")
                        .append(NameComponent.Name.c_str())
                        .append_convert(FString(" - (" + eastl::to_string(entt::to_integral(Entity)) + ")"));
                    OutlinerListView.Get<FTreeNodeDisplay>(It->second).DisplayName.assign(Label.data(), Label.length());
                }
                return true;
            }
    
            ImGui::SameLine();
    
            if (ImGui::Button("Cancel", ImVec2(ButtonWidth, 0.0f)))
            {
                return true;
            }

            return false;
        });
    }

    void FSceneEditorTool::DrawEntityProperties(entt::entity Entity)
    {
        const bool bMultiSelect = SelectedEntities.size() > 1 && IsEntitySelected(Entity);

        SNameComponent* NameComponent = GetSceneRegistry().try_get<SNameComponent>(Entity);
        FName EntityName = NameComponent ? NameComponent->Name : eastl::to_string((uint32)Entity);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

        constexpr ImGuiTableFlags Flags =
        ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_NoBordersInBodyUntilResize |
        ImGuiTableFlags_SizingFixedFit;

        if (ImGui::BeginTable("##EntityName", 1, Flags))
        {
            ImGui::TableSetupColumn("##Editor", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextColumn();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, EditorColors::U32(EditorColors::RowBg()));

            ImGui::BeginHorizontal(EntityName.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), 0.5f);

            ImGuiX::TextColoredUnformatted(EditorColors::Accent(), LE_ICON_CUBE);

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::LargeBold);
            if (bMultiSelect)
            {
                ImGuiX::Text("{} Entities Selected", (uint32)SelectedEntities.size());
            }
            else
            {
                ImGuiX::Text("{}", EntityName);
            }
            ImGui::PopFont();

            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextDim());
            if (bMultiSelect)
            {
                ImGuiX::Text("Editing {}", EntityName);
            }
            else
            {
                ImGuiX::Text("ID {}", entt::to_integral(Entity));
            }
            ImGui::PopStyleColor();

            ImGui::Spring(1.0f);

            const float ActionDim = ImGui::GetFrameHeight();
            const ImVec2 ActionSize(ActionDim, ActionDim);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));

            ImGui::PushStyleColor(ImGuiCol_Button, EditorColors::Success());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorColors::Lighten(EditorColors::Success(), 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorColors::WithAlpha(EditorColors::Success(), 0.85f));

            if (ImGui::Button(LE_ICON_PLUS, ActionSize))
            {
                AddEntityComponentFilter.Clear();
                ImGui::OpenPopup("AddToEntityMenu");
            }
            DrawAddToEntityOrWorldPopup(Entity);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGui::SetTooltip("Add Component");
            }

            // Tool-specific header buttons (world: Add Tag).
            DrawDetailsHeaderExtraButtons(Entity);

            ImGui::PopStyleColor(3);

            // One name, so renaming a multi-selection has no sensible meaning, and an entity without
            // a name component has nothing to write to.
            const bool bCanRename = !bMultiSelect && NameComponent != nullptr;
            ImGui::BeginDisabled(!bCanRename);

            if (ImGui::Button(LE_ICON_RENAME, ActionSize))
            {
                PushRenameEntityModal(Entity);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(bMultiSelect ? "Rename is single selection only" : "Rename Entity");
            }

            ImGui::EndDisabled();

            const bool bCanDelete = bMultiSelect || CanDeleteEntity(Entity);
            ImGui::BeginDisabled(!bCanDelete);
            ImGui::PushStyleColor(ImGuiCol_Button, EditorColors::Danger());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorColors::Lighten(EditorColors::Danger(), 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorColors::WithAlpha(EditorColors::Danger(), 0.85f));

            if (ImGui::Button(LE_ICON_TRASH_CAN, ActionSize))
            {
                if (bMultiSelect)
                {
                    if (Dialogs::Confirmation("Confirm Deletion",
                        "Are you sure you want to delete {0} selected entities?\n\nThis action cannot be undone.",
                        (uint32)SelectedEntities.size()))
                    {
                        for (entt::entity Selected : SelectedEntities)
                        {
                            if (CanDeleteEntity(Selected))
                            {
                                EntityDestroyRequests.push(Selected);
                            }
                        }
                    }
                }
                else if (Dialogs::Confirmation("Confirm Deletion",
                    "Are you sure you want to delete entity \"{0}\"?\n\nThis action cannot be undone.",
                    (uint32)Entity))
                {
                    EntityDestroyRequests.push(Entity);
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGui::SetTooltip(bMultiSelect ? "Delete Selected" : "Delete Entity");
            }

            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            ImGui::PopStyleVar();

            ImGui::EndHorizontal();
            ImGui::PopStyleVar(3);

            ImGui::EndTable();
        }

        ImGui::SeparatorText("Details");

        // Tool-specific sections above the component list (world: Tags).
        DrawDetailsExtraSections(Entity);

        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextDim());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(LE_ICON_CUBE " Components");
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        DrawComponentList(Entity);
    }

    void FSceneEditorTool::DrawEmptyState()
    {
        ImVec2 WindowSize = ImGui::GetWindowSize();
        ImVec2 CenterPos = ImVec2(WindowSize.x * 0.5f, WindowSize.y * 0.5f);

        ImGui::SetCursorPos(ImVec2(CenterPos.x - 100.0f, CenterPos.y - 40.0f));

        ImGui::BeginGroup();
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());

            const char* EmptyIcon = LE_ICON_INBOX;
            ImVec2 IconSize = ImGui::CalcTextSize(EmptyIcon);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (200.0f - IconSize.x) * 0.5f);

            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
            ImGui::TextUnformatted(EmptyIcon);
            ImGui::PopFont();

            ImGui::Spacing();

            const char* EmptyText = "Nothing selected";
            ImVec2 TextSize = ImGui::CalcTextSize(EmptyText);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (200.0f - TextSize.x) * 0.5f);
            ImGui::TextUnformatted(EmptyText);

            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::WithAlpha(EditorColors::TextMuted(), 0.7f));
            const char* HintText = "Select an entity to view properties";
            ImVec2 HintSize = ImGui::CalcTextSize(HintText);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (200.0f - HintSize.x) * 0.5f);
            ImGui::TextUnformatted(HintText);
            ImGui::PopStyleColor();

            ImGui::PopStyleColor();
        }
        ImGui::EndGroup();
    }

    size_t FSceneEditorTool::CountOutlinerEntities() const
    {
        FEntityRegistry& Registry = GetSceneRegistry();
        const auto& NameStorage = Registry.storage<SNameComponent>();
        
        size_t Count = NameStorage.size();
        for (entt::entity Hidden : Registry.view<FHideInSceneOutliner>())
        {
            if (NameStorage.contains(Hidden))
            {
                --Count;
            }
        }
        return Count;
    }

    void FSceneEditorTool::DrawFilterOptions()
    {
        using namespace entt::literals;

        if (ImGui::Button("Reset Filters", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
        {
            EntityFilterState.ComponentFilters.clear();
        }

        if (ImGui::BeginTable("ComponentFilters", 1,
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchSame |
            ImGuiTableFlags_ScrollY, ImVec2(0.0f, 400.0f)))
        {
            ImGui::TableSetupColumn("Component Type");
            ImGui::TableHeadersRow();

            for (auto&& [ID, Storage] : GetSceneRegistry().storage())
            {
                if (entt::meta_type MetaType = entt::resolve(Storage.info()))
                {
                    if (entt::meta_any ReturnValue = ECS::Utils::InvokeMetaFunc(MetaType, "static_struct"_hs))
                    {
                        CStruct* StructType = ReturnValue.cast<CStruct*>();

                        if (StructType->HasMeta("HideInComponentList"))
                        {
                            continue;
                        }

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();

                        auto It = eastl::find(EntityFilterState.ComponentFilters.begin(),
                            EntityFilterState.ComponentFilters.end(), StructType->GetName());

                        bool bIsFiltered = (It != EntityFilterState.ComponentFilters.end());
                        if (ImGui::Checkbox(StructType->MakeDisplayName().c_str(), &bIsFiltered))
                        {
                            if (bIsFiltered)
                            {
                                EntityFilterState.ComponentFilters.emplace_back(StructType->GetName());
                            }
                            else
                            {
                                EntityFilterState.ComponentFilters.erase(It);
                            }
                        }
                    }
                }
            }

            ImGui::EndTable();
        }
    }

    void FSceneEditorTool::DrawOutliner(bool bFocused)
    {
        // Track focus/hover so Delete (and other selection shortcuts) work from here too.
        bOutlinerActive = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
                       || ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

        {
            const ImGuiStyle& Style = ImGui::GetStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            const float ButtonWidth = ImGui::GetFrameHeight(); // square, matches the search field height
            
            const bool bForeign = IsInspectingForeignWorld();
            ImGui::BeginDisabled(bForeign);
            ImGui::PushStyleColor(ImGuiCol_Button, EditorColors::WithAlpha(EditorColors::Success(), 0.8f));
            if (ImGuiX::IconButton(LE_ICON_PLUS, "##AddToSceneGraph", 0xFFFFFFFF, ImVec2(ButtonWidth, ButtonWidth)))
            {
                ImGui::OpenPopup("AddToEntityMenu");
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(bForeign ? "Inspecting another world - switch back to add entities." : "Add a new entity.");
            }
            DrawAddToEntityOrWorldPopup();
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ButtonWidth - Style.FramePadding.x);
            EntityFilterState.FilterName.Draw("##Search");

            ImGui::PopStyleVar();

            if (!EntityFilterState.FilterName.IsActive())
            {
                ImDrawList* DrawList = ImGui::GetWindowDrawList();
                ImVec2 TextPos = ImGui::GetItemRectMin();
                TextPos.x += Style.FramePadding.x + 2.0f;
                TextPos.y += Style.FramePadding.y;
                DrawList->AddText(TextPos, EditorColors::U32(EditorColors::TextMuted()), LE_ICON_FILE_SEARCH " Search entities...");
            }

            ImGui::SameLine();

            const bool bFilterActive = EntityFilterState.FilterName.IsActive() || !EntityFilterState.ComponentFilters.empty();
            ImGui::PushStyleColor(ImGuiCol_Button,
                bFilterActive ? EditorColors::Accent() : EditorColors::Button());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                bFilterActive ? EditorColors::Lighten(EditorColors::Accent(), 0.12f) : EditorColors::Lighten(EditorColors::Button(), 0.1f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            if (ImGui::Button(LE_ICON_FILTER_SETTINGS "##ComponentFilter", ImVec2(ButtonWidth, ButtonWidth)))
            {
                ImGui::OpenPopup("FilterPopup");
            }

            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(bFilterActive ? "Filters active - Click to configure" : "Configure filters");
            }

            if (ImGui::BeginPopup("FilterPopup", ImGuiWindowFlags_NoMove))
            {
                ImGui::SeparatorText("Component Filters");
                DrawFilterOptions();
                ImGui::EndPopup();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        DrawOutlinerWorldSelector();

        {
            ImGui::Text(LE_ICON_FORMAT_LIST_NUMBERED " Total Entities: %s", eastl::to_string(CountOutlinerEntities()).c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 24 - ImGui::GetStyle().FramePadding.x);
            if (ImGui::Button(LE_ICON_REFRESH))
            {
                OutlinerListView.MarkTreeDirty();
            }

            ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorColors::PanelBg());
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
            if (ImGui::BeginChild("EntityList", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar))
            {
                LUMINA_PROFILE_SECTION("Draw Entity List");
                FlushOutlinerPending();
                OutlinerListView.Draw(OutlinerContext);

                if (ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->Rect(), ImGui::GetCurrentWindow()->ID))
                {
                    HandleOutlinerEmptyAreaDrop();
                    ImGui::EndDragDropTarget();
                }
            }
            ImGui::EndChild();

            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
    }

    void FSceneEditorTool::DrawAddToEntityOrWorldPopup(entt::entity Entity)
    {
        using namespace entt::literals;
        
        ImGui::SetNextWindowViewport(ImGui::GetWindowViewport()->ID);
        ImGui::SetNextWindowSize(ImVec2(450.0f, 550.0f), ImGuiCond_Always);

        if (ImGui::BeginPopup("AddToEntityMenu", ImGuiWindowFlags_NoMove))
        {
            if (Entity == entt::null)
            {
                ImGui::TextColored(EditorColors::SectionHeader(), LE_ICON_PLUS " Create New Entity");
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            }
            else if (SelectedEntities.size() > 1 && IsEntitySelected(Entity))
            {
                ImGui::TextColored(EditorColors::AccentAlt(), LE_ICON_SELECT_GROUP " Add to %llu selected entities",
                    (unsigned long long)SelectedEntities.size());
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            }

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::SetNextItemWidth(-1);
            AddEntityComponentFilter.Draw("##Search");
            if (ImGui::IsWindowAppearing())
            {
                AddEntityComponentFilter.Clear();
                ImGui::SetKeyboardFocusHere(-1);
            }
            if (!AddEntityComponentFilter.IsActive())
            {
                ImGuiStyle& Style = ImGui::GetStyle();
                ImDrawList* DrawList = ImGui::GetWindowDrawList();
                ImVec2 TextPos = ImGui::GetItemRectMin();
                TextPos.x += Style.FramePadding.x + 2.0f;
                TextPos.y += Style.FramePadding.y;
                DrawList->AddText(TextPos, EditorColors::U32(EditorColors::TextMuted()), LE_ICON_FOLDER_SEARCH " Search components...");
            }
            ImGui::PopStyleVar();

            ImGui::Spacing();

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));

            if (ImGui::BeginChild("TemplateList", ImVec2(0, -35.0f), true))
            {
                const bool bFiltering = AddEntityComponentFilter.IsActive();

                if (Entity == entt::null)
                {
                    static const FName PrefabClassName = FName("CPrefab");
                    TVector<FAssetData*> PrefabAssets = FAssetRegistry::Get().FindByPredicate([](const FAssetData& Data)
                    {
                        return Data.AssetClass == PrefabClassName;
                    });

                    if (!PrefabAssets.empty())
                    {
                        TVector<FAssetData*> FilteredPrefabs;
                        FilteredPrefabs.reserve(PrefabAssets.size());
                        for (FAssetData* Data : PrefabAssets)
                        {
                            if (AddEntityComponentFilter.PassFilter(Data->AssetName.c_str()))
                            {
                                FilteredPrefabs.push_back(Data);
                            }
                        }

                        eastl::sort(FilteredPrefabs.begin(), FilteredPrefabs.end(), [](FAssetData* LHS, FAssetData* RHS)
                        {
                            return LHS->AssetName.ToString() < RHS->AssetName.ToString();
                        });

                        if (!FilteredPrefabs.empty())
                        {
                            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 3.0f));
                            if (BeginPickerSection(LE_ICON_PACKAGE_VARIANT_CLOSED, "Prefabs", (int32)FilteredPrefabs.size(), bFiltering)
                                && ImGui::BeginTable("##Prefabs", 2, GPickerTableFlags))
                            {
                                ImGui::TableSetupColumn("##Icon", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                                ImGui::TableSetupColumn("##Name", ImGuiTableColumnFlags_WidthStretch);

                                for (FAssetData* Data : FilteredPrefabs)
                                {
                                    if (DrawPickerRow(Data, LE_ICON_PACKAGE_VARIANT_CLOSED, Data->AssetName.c_str()))
                                    {
                                        HandlePrefabContentDrop(FStringView(Data->Path.c_str()), entt::null, /*bAttachToTarget*/ false);
                                        ImGui::CloseCurrentPopup();
                                        AddEntityComponentFilter.Clear();
                                    }
                                }

                                ImGui::EndTable();
                            }
                            ImGui::PopStyleVar();
                        }
                    }
                }

                {
                    struct FPrimitiveEntry
                    {
                        const char* Icon;
                        const char* Name;
                        CStaticMesh* (*GetMesh)();
                    };

                    static const FPrimitiveEntry PrimitiveEntries[] =
                    {
                        { LE_ICON_CUBE,         "Cube",     []() -> CStaticMesh* { return CPrimitiveManager::Get().CubeMesh; } },
                        { LE_ICON_CIRCLE,       "Sphere",   []() -> CStaticMesh* { return CPrimitiveManager::Get().SphereMesh; } },
                        { LE_ICON_SQUARE,       "Plane",    []() -> CStaticMesh* { return CPrimitiveManager::Get().PlaneMesh; } },
                        { LE_ICON_CYLINDER,     "Cylinder", []() -> CStaticMesh* { return CPrimitiveManager::Get().CylinderMesh; } },
                        { LE_ICON_CONE,         "Cone",     []() -> CStaticMesh* { return CPrimitiveManager::Get().ConeMesh; } },
                        { LE_ICON_GAS_CYLINDER, "Capsule",  []() -> CStaticMesh* { return CPrimitiveManager::Get().CapsuleMesh; } },
                    };

                    TVector<const FPrimitiveEntry*> FilteredPrimitives;
                    FilteredPrimitives.reserve(IM_ARRAYSIZE(PrimitiveEntries));
                    for (const FPrimitiveEntry& Entry : PrimitiveEntries)
                    {
                        if (AddEntityComponentFilter.PassFilter(Entry.Name))
                        {
                            FilteredPrimitives.push_back(&Entry);
                        }
                    }

                    if (!FilteredPrimitives.empty())
                    {
                        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 3.0f));
                        if (BeginPickerSection(LE_ICON_SHAPE, "Primitives", (int32)FilteredPrimitives.size(), bFiltering)
                            && ImGui::BeginTable("##Primitives", 2, GPickerTableFlags))
                        {
                            ImGui::TableSetupColumn("##Icon", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                            ImGui::TableSetupColumn("##Name", ImGuiTableColumnFlags_WidthStretch);

                            for (const FPrimitiveEntry* Entry : FilteredPrimitives)
                            {
                                if (DrawPickerRow(Entry, Entry->Icon, Entry->Name))
                                {
                                    CStaticMesh* PrimitiveMesh = Entry->GetMesh();
                                    if (GetSceneRegistry().valid(Entity))
                                    {
                                        if (PrimitiveMesh != nullptr)
                                        {
                                            BeginTransaction();
                                            SStaticMeshComponent& MeshComp = GetSceneRegistry().emplace_or_replace<SStaticMeshComponent>(Entity);
                                            MeshComp.SetStaticMesh(PrimitiveMesh);
                                            EndTransaction("Set Primitive Mesh");

                                            OutlinerListView.MarkTreeDirty();
                                            if (Entity == DetailsEntity)
                                            {
                                                bDetailsDirty = true;
                                            }
                                        }
                                    }
                                    else
                                    {
                                        CreatePrimitiveEntity(PrimitiveMesh, Entry->Name);
                                    }

                                    ImGui::CloseCurrentPopup();
                                    AddEntityComponentFilter.Clear();
                                }
                            }

                            ImGui::EndTable();
                        }
                        ImGui::PopStyleVar();
                    }
                }

                entt::meta_type       PickedMetaType;
                CStruct*              PickedStruct = nullptr;

                // Resolved before the list is drawn so it can grey out what these targets already have.
                TVector<entt::entity> Targets = GetComponentEditTargets(Entity);

                if (DrawAddableComponentList(AddEntityComponentFilter, Targets, PickedMetaType, PickedStruct))
                {
                    if (!Targets.empty())
                    {
                        ApplyAddComponentToTargets(Targets, PickedMetaType);
                    }
                    else
                    {
                        CreateEntityWithComponent(PickedStruct);
                    }

                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndChild();

            ImGui::PopStyleVar(2);

            ImGui::Separator();

            ImGui::BeginGroup();
            {
                ImGui::PushStyleColor(ImGuiCol_Button, EditorColors::Danger());
                if (ImGui::Button("Cancel", ImVec2(80.0f, 0.0f)))
                {
                    ImGui::CloseCurrentPopup();
                    AddEntityComponentFilter.Clear();
                }
                ImGui::PopStyleColor();

                if (Entity == entt::null)
                {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, EditorColors::Button());
                    if (ImGui::Button(LE_ICON_CUBE " Empty Entity", ImVec2(-1, 0.0f)))
                    {
                        CreateEntity();
                        ImGui::CloseCurrentPopup();
                        AddEntityComponentFilter.Clear();
                    }
                    ImGui::PopStyleColor();

                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("Create entity without any components");
                    }
                }
            }
            ImGui::EndGroup();

            ImGui::EndPopup();
        }
    }
}
