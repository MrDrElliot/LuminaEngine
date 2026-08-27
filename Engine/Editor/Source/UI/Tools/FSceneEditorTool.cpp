#include "TaskSystem/ParallelSort.h"
#include "FSceneEditorTool.h"
#include "World/ECS/Registry.h"
#include <algorithm>
#include "Scripting/DotNet/DotNetHost.h"


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
#include "Input/InputViewport.h"
#include "Renderer/ViewVolume.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "Animation/SkeletalMeshUtils.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Tools/ComponentVisualizers/ComponentVisualizer.h"
#include "Tools/Dialogs/Dialogs.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "UI/Properties/PropertyEditContexts.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "Scene/SceneOps.h"
#include "UI/Tools/EditorEntityUtils.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/SceneFolderComponent.h"
#include "World/Entity/Components/SingletonEntityComponent.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/TagComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/EntityUtils.h"
#include "World/World.h"
#include "World/Entity/Components/LightComponent.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    // Neither subclass uses the inherited PropertyTable, and FEditorTool's ctor only stores World.
    FSceneEditorTool::FSceneEditorTool(IEditorToolContext* Context, const FString& DisplayName, CObject* InAsset, CWorld* InWorld)
        : FAssetEditorTool(Context, DisplayName)
    {
        PickCtx.Broker = MakeShared<FEntityPickBroker>();
        PropertyContext.Provide(&SocketCtx);
        PropertyContext.Provide(&AnimGraphCtx);
        PropertyContext.Provide(&WorldCtx);
        PropertyContext.Provide(&PickCtx);
        Asset = InAsset;
        World = InWorld;
    }

    FSceneEditorTool::FSceneEditorTool(IEditorToolContext* Context, const FString& DisplayName, CWorld* InWorld)
        : FAssetEditorTool(Context, DisplayName)
    {
        PickCtx.Broker = MakeShared<FEntityPickBroker>();
        PropertyContext.Provide(&SocketCtx);
        PropertyContext.Provide(&AnimGraphCtx);
        PropertyContext.Provide(&WorldCtx);
        PropertyContext.Provide(&PickCtx);

        // The editor owns the world's lifetime, so a second owning ref here would corrupt the refcount.
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

    void FSceneEditorTool::SetSingleSelectedEntity(ECS::FEntity Entity)
    {
        ECS::FRegistry& Registry = GetSceneRegistry();
        if (Entity != ECS::NullEntity && !Registry.IsValid(Entity))
        {
            Entity = ECS::NullEntity;
        }

        // Clicking the already-singularly-selected entity is a no-op.
        if (Entity == LastSelectedEntity && SelectedEntities.size() == (Entity == ECS::NullEntity ? 0 : 1)
            && (Entity == ECS::NullEntity || SelectedEntities.find(Entity) != SelectedEntities.end()))
        {
            return;
        }

        // Drop tags from entities no longer selected so render highlighting matches the canonical set.
        for (ECS::FEntity Old : SelectedEntities)
        {
            if (Old != Entity && Registry.IsValid(Old))
            {
                Registry.Remove<FSelectedInEditorComponent>(Old);
                SyncOutlinerRowSelection(Old, false);
            }
        }
        SelectedEntities.clear();

        // Clear last-selected tag unconditionally; re-emplace below if new selection isn't empty.
        Registry.ClearComponent<FLastSelectedTag>();

        if (Entity != ECS::NullEntity)
        {
            SelectedEntities.insert(Entity);
            Registry.EmplaceOrReplace<FSelectedInEditorComponent>(Entity);
            Registry.EmplaceOrReplace<FLastSelectedTag>(Entity);
            SyncOutlinerRowSelection(Entity, true);
        }

        if (LastSelectedEntity != Entity)
        {
            LastSelectedEntity = Entity;
            OnSelectionChanged();
        }
    }

    void FSceneEditorTool::AddSelectedEntity(ECS::FEntity Entity, bool /*bRebuild*/)
    {
        ECS::FRegistry& Registry = GetSceneRegistry();
        if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
        {
            return;
        }

        const bool bWasAlreadySelected = SelectedEntities.find(Entity) != SelectedEntities.end();
        if (!bWasAlreadySelected)
        {
            SelectedEntities.insert(Entity);
            Registry.EmplaceOrReplace<FSelectedInEditorComponent>(Entity);
            SyncOutlinerRowSelection(Entity, true);
        }

        // Always promote to last-selected so clicking a row in a multi-select focuses details.
        if (LastSelectedEntity != Entity)
        {
            Registry.ClearComponent<FLastSelectedTag>();
            Registry.EmplaceOrReplace<FLastSelectedTag>(Entity);
            LastSelectedEntity = Entity;
            OnSelectionChanged();
        }
    }

    void FSceneEditorTool::RemoveSelectedEntity(ECS::FEntity Entity, bool /*bRebuild*/)
    {
        if (Entity == ECS::NullEntity)
        {
            return;
        }

        auto SetIt = SelectedEntities.find(Entity);
        if (SetIt == SelectedEntities.end())
        {
            return;
        }

        SelectedEntities.erase(SetIt);

        ECS::FRegistry& Registry = GetSceneRegistry();
        if (Registry.IsValid(Entity))
        {
            Registry.Remove<FSelectedInEditorComponent>(Entity);
        }

        SyncOutlinerRowSelection(Entity, false);

        // If the deselected entity was the focus, pick a new one so "last" isn't stale.
        if (LastSelectedEntity == Entity)
        {
            Registry.ClearComponent<FLastSelectedTag>();
            ECS::FEntity NewLast = ECS::NullEntity;
            for (ECS::FEntity Candidate : SelectedEntities)
            {
                if (Registry.IsValid(Candidate))
                {
                    NewLast = Candidate;
                    break;
                }
            }
            if (NewLast != ECS::NullEntity)
            {
                Registry.EmplaceOrReplace<FLastSelectedTag>(NewLast);
            }
            LastSelectedEntity = NewLast;
            OnSelectionChanged();
        }
    }

    void FSceneEditorTool::ToggleSelectedEntity(ECS::FEntity Entity)
    {
        ECS::FRegistry& Registry = GetSceneRegistry();
        if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
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
        for (ECS::FEntity Old : SelectedEntities)
        {
            SyncOutlinerRowSelection(Old, false);
        }
        SelectedEntities.clear();
        LastSelectedEntity = ECS::NullEntity;

        ECS::FRegistry& Registry = GetSceneRegistry();

        Registry.View<FSelectedInEditorComponent>().ForEach([&](ECS::FEntity Entity)
        {
            SelectedEntities.insert(Entity);
            SyncOutlinerRowSelection(Entity, true);
        });

        // FLastSelectedTag should be serialized; fall back to first selected if it's missing.
        Registry.View<FLastSelectedTag>().ForEach([&](ECS::FEntity Entity)
        {
            LastSelectedEntity = Entity;
        });

        if (LastSelectedEntity == ECS::NullEntity && !SelectedEntities.empty())
        {
            ECS::FEntity First = *SelectedEntities.begin();
            LastSelectedEntity = First;
            Registry.EmplaceOrReplace<FLastSelectedTag>(First);
        }

        OnSelectionChanged();
    }

    void FSceneEditorTool::ReapplySelectionTags()
    {
        ECS::FRegistry& Registry = GetSceneRegistry();
        for (ECS::FEntity Entity : SelectedEntities)
        {
            if (Registry.IsValid(Entity))
            {
                Registry.EmplaceOrReplace<FSelectedInEditorComponent>(Entity);
            }
        }
        if (LastSelectedEntity != ECS::NullEntity && Registry.IsValid(LastSelectedEntity))
        {
            Registry.EmplaceOrReplace<FLastSelectedTag>(LastSelectedEntity);
        }
    }

    void FSceneEditorTool::ClearSelectedEntities()
    {
        ECS::FRegistry& Registry = GetSceneRegistry();

        for (ECS::FEntity Entity : SelectedEntities)
        {
            SyncOutlinerRowSelection(Entity, false);
        }

        SelectedEntities.clear();

        // Bulk-erase via registry clear<>(); cheaper than walking SelectedEntities.
        Registry.ClearComponent<FSelectedInEditorComponent>();
        Registry.ClearComponent<FLastSelectedTag>();

        if (LastSelectedEntity != ECS::NullEntity)
        {
            LastSelectedEntity = ECS::NullEntity;
            OnSelectionChanged();
        }
    }

    void FSceneEditorTool::ResetSelectionState()
    {
        SelectedEntities.clear();
        LastSelectedEntity = ECS::NullEntity;

        if (CWorld* Observed = GetObservedWorld())
        {
            ECS::FRegistry& Registry = ECS::GetWorldRegistry(*Observed);
            Registry.ClearComponent<FSelectedInEditorComponent>();
            Registry.ClearComponent<FLastSelectedTag>();
        }
    }

    bool FSceneEditorTool::IsComponentHiddenInDetails(const CStruct* Type) const
    {
        // HideInComponentList means cannot be hand-added, which is a different question from this.
        return Type != nullptr && Type->HasMeta("HideInDetails");
    }

    void FSceneEditorTool::DrawComponentSearchBar()
    {
        // Each table restores its own collapse state when its filter goes inactive.
        ImGuiX::SearchBar("##DetailsSearch", DetailsFilter, LE_ICON_MAGNIFY " Search components and properties...");

        ImGui::Spacing();
    }

    void FSceneEditorTool::DrawComponentList(ECS::FEntity Entity)
    {
        const bool bFiltering = DetailsFilter.IsActive();
        uint32 VisibleCount = 0;

        for (FComponentTableEntry& Entry : PropertyTables)
        {
            if (bFiltering)
            {
                // A name match shows the component in full, since that is what searching for it means.
                const bool bTitleMatches = ImGuiX::PassSearchFilter(DetailsFilter, Entry.Title.c_str());

                if (Entry.Table)
                {
                    Entry.Table->SetSearchText(bTitleMatches ? FStringView() : FStringView(DetailsFilter.InputBuf));

                    // Drawing an empty header leaves sections to scroll past, which is what a search removes.
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

    void FSceneEditorTool::DrawComponentHeader(FComponentTableEntry& Entry, ECS::FEntity Entity)
    {

        // Existence check via meta; drop this row if the entity no longer holds the component.
        if (IsComponentHiddenInDetails(Entry.ReflectedType))
        {
            return;
        }

        if (!ECS::Utils::HasComponent(GetSceneRegistry(), Entity, Entry.ReflectedType))
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

            // Makes the world resolvable to an entity picker, and the parent's sockets to a socket picker.
            SocketCtx = FSocketEditContext();
            BuildSocketPickerData(Entity, SocketCtx);
            WorldCtx.World = World;

            // Lets a parameter picker on a component list the graph that entity actually runs.
            const SAnimationGraphComponent* AnimGraph = GetSceneRegistry().TryGet<SAnimationGraphComponent>(Entity);
            AnimGraphCtx.Graph = (AnimGraph != nullptr) ? AnimGraph->Graph.Get() : nullptr;

            Entry.Table->SetContext(&PropertyContext);
            Entry.Table->DrawTree();

            ImGui::Unindent(8.0f);

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);

            ImGui::Spacing();
        }

        ImGui::PopID();
    }

    void FSceneEditorTool::BuildSocketPickerData(ECS::FEntity Entity, FSocketEditContext& Out)
    {
        ECS::FRegistry& Registry = GetSceneRegistry();

        const FRelationshipComponent* Relationship = Registry.TryGet<FRelationshipComponent>(Entity);
        if (Relationship == nullptr || Relationship->Parent == ECS::NullEntity || !Registry.IsValid(Relationship->Parent))
        {
            return;
        }

        if (const SSkeletalMeshComponent* SkeletalMesh = Registry.TryGet<SSkeletalMeshComponent>(Relationship->Parent))
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
        else if (const SStaticMeshComponent* StaticMesh = Registry.TryGet<SStaticMeshComponent>(Relationship->Parent))
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

    void FSceneEditorTool::RemoveComponent(ECS::FEntity Entity, const CStruct* ComponentType)
    {
        bool bWasRemoved = false;

        if (ComponentType == nullptr)
        {
            return;
        }

        ECS::Utils::ForEachComponent(GetSceneRegistry(), Entity, [&](void*, ECS::FSparseSet& Set, CStruct* StructType)
        {
            if (StructType == ComponentType)
            {
                Set.RemoveEntity(Entity);
                bWasRemoved = true;
            }
        });


        if (bWasRemoved)
        {
            // The prefab asset is untouched, so the inherited versus added test stays valid.
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
        if (ComponentDestroyRequests.empty())
        {
            return;
        }

        // Drained first, since the commands have to read the components while they are still attached.
        TVector<FComponentDestroyRequest> Requests;
        while (!ComponentDestroyRequests.empty())
        {
            Requests.push_back(ComponentDestroyRequests.front());
            ComponentDestroyRequests.pop();
        }

        TVector<const CStruct*> Types;
        TVector<ECS::FEntity>   AllEntities;
        for (const FComponentDestroyRequest& Request : Requests)
        {
            if (Request.Type != nullptr)
            {
                Types.AddUnique(Request.Type);
            }
            AllEntities.AddUnique(Request.EntityID);
        }

        // One command per distinct type, since each records a different component.
        bool bOpened = false;
        for (const CStruct* Type : Types)
        {
            TVector<ECS::FEntity> Targets;
            for (const FComponentDestroyRequest& Request : Requests)
            {
                if (Request.Type == Type)
                {
                    Targets.AddUnique(Request.EntityID);
                }
            }

            if (bOpened)
            {
                RecordComponentSnapshot(Targets, const_cast<CStruct*>(Type));
            }
            else
            {
                BeginComponentTransaction(Targets, const_cast<CStruct*>(Type));
                bOpened = true;
            }
        }

        if (!bOpened)
        {
            BeginTransaction();
        }

        RecordPrefabOverrideSnapshot(AllEntities);

        for (const FComponentDestroyRequest& Request : Requests)
        {
            RemoveComponent(Request.EntityID, Request.Type);
        }

        EndTransaction("Remove Component");
    }

    void FSceneEditorTool::OnPrePropertyChangeEvent(const FPropertyChangedEvent& Event)
    {
    }

    void FSceneEditorTool::BeginPropertyEditTransaction(CStruct* ComponentType)
    {
        ECS::FRegistry& Registry = GetSceneRegistry();

        // Exactly the set OnPostPropertyChangeEvent writes, the focus plus the selection it replicates to.
        TVector<ECS::FEntity> Targets;
        if (Registry.IsValid(DetailsEntity))
        {
            Targets.push_back(DetailsEntity);
        }

        Registry.View<FSelectedInEditorComponent>().ForEach([&](ECS::FEntity Entity)
        {
            if (Entity != DetailsEntity)
            {
                Targets.push_back(Entity);
            }
        });

        BeginComponentTransaction(Targets, ComponentType);
    }

    void FSceneEditorTool::OnPostPropertyChangeEvent(const FPropertyChangedEvent& Event)
    {

        // A reset can fire with no outer component type, so bail before dereferencing it.
        if (Event.OuterType == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = GetSceneRegistry();
        CStruct* WantType = Event.OuterType;
        const FComponentOps* WantOps = WantType->GetComponentOps();
        if (WantOps == nullptr)
        {
            return;
        }

        // Fires on every op, not just the commit, so scrubbing a transform field tracks live in the viewport.
        const bool bTransformEdited = Event.OuterType == STransformComponent::StaticStruct();

        // Null for script-defined structs, which carry no compile-time struct ops.
        FStructOps* PeerOps = Event.OuterType->GetStructOps();

        // The focus entity is what the bound table edited, so locate its instance to replicate from.
        void* FocusInstance = nullptr;
        if (Event.Property != nullptr && Registry.IsValid(DetailsEntity))
        {
            FocusInstance = WantOps->Get(Registry, DetailsEntity);
        }

        auto View = Registry.View<FSelectedInEditorComponent>();
        View.ForEach([&](ECS::FEntity Entity)
        {
            if (WantOps->Has(Registry, Entity) == 0)
            {
                return;
            }

            // Copy the edited property from the focus instance onto this one.
            if (FocusInstance != nullptr && Entity != DetailsEntity)
            {
                void* DestInstance = WantOps->Get(Registry, Entity);

                // Offset-addressed, since a nested row's property is offset within its own struct.
                if (DestInstance != nullptr && DestInstance != FocusInstance && Event.ValueOffset >= 0)
                {
                    Event.Property->CopyCompleteValue(static_cast<uint8*>(DestInstance) + Event.ValueOffset,
                                                      static_cast<const uint8*>(FocusInstance) + Event.ValueOffset);

                    // The copy above is a raw store, so the peer needs the commit hook the focus instance got.
                    if (Event.bIsCommit && PeerOps != nullptr && PeerOps->HasPostEdit())
                    {
                        PeerOps->PostEdit(DestInstance, Event);
                    }
                }
            }

            // The same bypass as the focus instance, since the copy above is a raw property store.
            if (bTransformEdited)
            {
                Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
            }

            // A back-to-prefab edit leaves zero divergent leaves, which clears the override record.
            CPrefab::RecaptureComponentOverrides(Registry, Entity, Event.OuterType);
        });
    }

    void FSceneEditorTool::RebuildPropertyTables(ECS::FEntity Entity)
    {

        PropertyTables.clear();

        // Track owning entity so the details panel can detect staleness; null on invalid input forces a rebuild next time.
        DetailsEntity = (Entity != ECS::NullEntity && GetSceneRegistry().IsValid(Entity)) ? Entity : ECS::NullEntity;
        bDetailsDirty = false;

        if (GetSceneRegistry().IsValid(Entity))
        {
            ECS::FRegistry& Registry = GetSceneRegistry();

            // One intermediate row per reflected component.
            struct FPendingRow
            {
                void*                 Data = nullptr;
                CStruct*              Layout = nullptr;        // reflected CStruct
                const CStruct*        ReflectedType = nullptr;
                FString               Title;
                TVector<void*>        OtherInstances;          // same component on the other selected entities (multi-edit)
            };

            // Shows the intersection of components and compares each value across the whole selection.
            const bool bMultiSelect = SelectedEntities.size() > 1 && IsEntitySelected(Entity);

            TVector<ECS::FEntity> OtherTargets;
            TVector<THashMap<const CStruct*, void*>> OtherReflected;
            if (bMultiSelect)
            {
                for (ECS::FEntity Selected : SelectedEntities)
                {
                    if (Selected == Entity || !Registry.IsValid(Selected))
                    {
                        continue;
                    }
                    OtherTargets.push_back(Selected);

                    THashMap<const CStruct*, void*> ReflectedMap;
                    ECS::Utils::ForEachComponent(Registry, Selected, [&](void* Component, ECS::FSparseSet&, CStruct* Struct)
                    {
                        ReflectedMap[Struct] = Component;
                    });
                    OtherReflected.push_back(Move(ReflectedMap));
                }
            }

            TVector<FPendingRow> Pending;

            ECS::Utils::ForEachComponent(Registry, Entity, [&](void* Component, ECS::FSparseSet& Set, CStruct* Struct)
            {
                // Components that never show in the details panel (tags, prefab bookkeeping).
                if (IsComponentHiddenInDetails(Struct))
                {
                    return;
                }

                // Drops components not on every selected entity, gathering pointers for the multi-value compare.
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

            Algo::Sort(Pending.begin(), Pending.end(), [&](const FPendingRow& LHS, const FPendingRow& RHS)
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

            // A prefab instance compares against the prefab baseline rather than the class CDO.
            SPrefabInstanceComponent* FocusPrefabInstance = Registry.TryGet<SPrefabInstanceComponent>(Entity);

            // A source prefab can be a marked-destroy zombie, which a plain null test does not catch.
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

                // Everything that is not a prefab instance falls back to the struct CDO.
                void* PrefabDefault = nullptr;
                if (FocusSourcePrefab != nullptr && Row.Layout != nullptr)
                {
                    PrefabDefault = FocusSourcePrefab->ResolvePrefabComponentPtr(FocusPrefabInstance->StableID, Row.Layout);
                }

                Entry.Table = (PrefabDefault != nullptr)
                    ? MakeUnique<FPropertyTable>(Row.Data, Row.Layout, PrefabDefault)
                    : MakeUnique<FPropertyTable>(Row.Data, Row.Layout);
                // The panel owns ONE search box, or a per-table box would sit above each component header.
                Entry.Table->SetShowSearchBar(false);
                Entry.Table->SetPreEditCallback([&](const FPropertyChangedEvent& Event)    { OnPrePropertyChangeEvent(Event); });
                Entry.Table->SetPostEditCallback([&](const FPropertyChangedEvent& Event)   { OnPostPropertyChangeEvent(Event); MarkSceneDirty(); });
                Entry.Table->SetStartEditCallback([this, Layout = Row.Layout](const FPropertyChangedEvent& Event) { BeginPropertyEditTransaction(Layout); });
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

    bool FSceneEditorTool::IsOutlinerEntityVisible(ECS::FEntity Entity) const
    {
        ECS::FRegistry& Registry = GetSceneRegistry();
        return Registry.IsValid(Entity)
            && Registry.HasAll<SNameComponent>(Entity)
            && !Registry.HasAny<FHideInSceneOutliner>(Entity);
    }

    void FSceneEditorTool::SyncOutlinerRowSelection(ECS::FEntity Entity, bool bSelected)
    {
        auto It = EntityToTreeNode.find(Entity);
        if (It != EntityToTreeNode.end() && OutlinerListView.IsValid(It->second))
        {
            OutlinerListView.Get<FTreeNodeState>(It->second).bSelected = bSelected;
        }
    }

    void FSceneEditorTool::RebuildSceneOutliner(FTreeListView& Tree)
    {
        // A rebuild resets the map and re-adds roots, and children fill lazily on expand.
        EntityToTreeNode.clear();
        PendingOutlinerAdds.clear();
        PendingExpanderRefresh.clear();

        BuildFolderNodes(Tree);

        ECS::FRegistry& Registry = GetSceneRegistry();
        auto View = Registry.View<SNameComponent>(ECS::TExclude<FHideInSceneOutliner>{});
        
        TVector<ECS::FEntity> Roots;
        Roots.reserve(View.Num());
        for (ECS::FEntity Entity : View)
        {
            if (!IsOutlinerEntityVisible(Entity))
            {
                continue;
            }
            if (FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Entity))
            {
                if (Rel->Parent != ECS::NullEntity)
                {
                    continue;
                }
            }

            Roots.push_back(Entity);
        }

        Task::ParallelSort(Roots.begin(), Roots.end(), [&](ECS::FEntity LHS, ECS::FEntity RHS)
        {
            const FName& A = View.Get<SNameComponent>(LHS).Name;
            const FName& B = View.Get<SNameComponent>(RHS).Name;

            if (A != B)
            {
                return A < B;
            }
            
            return LHS < RHS;
        });

        for (ECS::FEntity Root : Roots)
        {
            AddEntityToOutliner(Root);
        }
    }

    FTreeNodeID FSceneEditorTool::AddEntityToOutliner(ECS::FEntity Entity)
    {
        ECS::FRegistry& Registry = GetSceneRegistry();
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
        bool bHasEntityParent = false;
        if (FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Entity))
        {
            if (Rel->Parent != ECS::NullEntity)
            {
                bHasEntityParent = true;

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

        // Only unparented entities are filed in folders; an attached one lives under its parent's row.
        if (!bHasEntityParent)
        {
            const uint32 FolderID = GetEntityFolderID(Entity);
            ParentNode = FindFolderNode(FolderID);

            if (FolderID != SSceneFolderComponent::NoFolder && !ParentNode.IsValid())
            {
                OutlinerListView.MarkTreeDirty();
            }
        }

        SNameComponent& NameComponent = Registry.Get<SNameComponent>(Entity);
        const SPrefabInstanceComponent* PrefabInstance = Registry.TryGet<SPrefabInstanceComponent>(Entity);
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
        Name.append(NameComponent.Name.c_str()).append(" (").append(Format("{}", (Entity).Value) + ")");

        FTreeNodeID ItemEntity = OutlinerListView.CreateNode(ParentNode, FStringView(Name.data(), Name.length()));
        EntityToTreeNode[Entity] = ItemEntity;

        FTreeNodeDisplay& Display = OutlinerListView.Get<FTreeNodeDisplay>(ItemEntity);

        // Tint the entity's box glyph with the editor accent so plain entities stand out in the outliner.
        if (!bIsPrefabInstanceRoot && !bIsLockedPrefabChild)
        {
            Display.IconText = LE_ICON_CUBE;
            Display.IconColor = EditorColors::EntityIcon();
        }

        // The component scan dominates the per-row cost, so tooltips build lazily on first hover.

        Display.bShowDisabledIcon = true;
        Display.bAllowRenaming = !bIsLockedPrefabChild;

        // The per-entity script toggle is only shown when the entity carries a script.
        if (Registry.HasAny<SEntityScriptComponent>(Entity))
        {
            Display.bShowSecondaryIcon = true;
            Display.SecondaryIconOn    = LE_ICON_SCRIPT_TEXT;
            Display.SecondaryIconOff   = LE_ICON_SCRIPT_TEXT_OUTLINE;
            Display.SecondaryTooltip   = "Toggle this entity's script on/off (the entity stays active).";
        }

        OutlinerListView.EmplaceUserData<FEntityListViewItemData>(ItemEntity).Entity = Entity;

        if (Registry.HasAny<FSelectedInEditorComponent>(Entity))
        {
            OutlinerListView.Get<FTreeNodeState>(ItemEntity).bSelected = true;
        }

        if (Registry.HasAny<SDisabledTag>(Entity))
        {
            OutlinerListView.Get<FTreeNodeState>(ItemEntity).bDisabled = true;
        }

        if (Registry.HasAny<SScriptDisabledTag>(Entity))
        {
            OutlinerListView.Get<FTreeNodeState>(ItemEntity).bSecondaryToggled = true;
        }

        // Only show an expander if the entity actually has child entities; lazy expansion populates them.
        const FRelationshipComponent* RelForChildren = Registry.TryGet<FRelationshipComponent>(Entity);
        const bool bHasChildren = RelForChildren != nullptr && RelForChildren->Children > 0;
        OutlinerListView.MarkHasLazyChildren(ItemEntity, bHasChildren);

        return ItemEntity;
    }

    void FSceneEditorTool::BuildEntityTooltip(ECS::FEntity Entity, FTreeNodeDisplay& Display)
    {
        if (Display.bTooltipBuilt)
        {
            return;
        }

        ECS::FRegistry& Registry = GetSceneRegistry();
        if (!Registry.IsValid(Entity) || !Registry.HasAll<SNameComponent>(Entity))
        {
            return;
        }

        const SNameComponent& NameComponent = Registry.Get<SNameComponent>(Entity);
        const SPrefabInstanceComponent* PrefabInstance = Registry.TryGet<SPrefabInstanceComponent>(Entity);
        const bool bIsPrefabInstanceRoot = PrefabInstance != nullptr && PrefabInstance->bIsRoot;
        const bool bIsLockedPrefabChild = PrefabInstance != nullptr && !PrefabInstance->bIsRoot;

        // A styled tooltip of type icon and name, a dim subtitle, then component chips.
        const char* TypeIcon = bIsPrefabInstanceRoot ? LE_ICON_PACKAGE_VARIANT_CLOSED
                             : bIsLockedPrefabChild   ? LE_ICON_LOCK
                                                      : LE_ICON_CUBE;
        Display.TooltipTitle = FString(TypeIcon) + " " + NameComponent.Name.c_str();

        if (bIsLockedPrefabChild)
        {
            Display.TooltipSubtitle = FString("Prefab child #" + Format("{}", (Entity).Value) + ", hierarchy locked");
        }
        else if (bIsPrefabInstanceRoot)
        {
            Display.TooltipSubtitle = FString("Prefab instance #" + Format("{}", (Entity).Value));
        }
        else
        {
            Display.TooltipSubtitle = FString("Entity #" + Format("{}", (Entity).Value));
        }

        Display.TooltipChipHeader = "COMPONENTS";
        Display.TooltipChips.clear();
        ECS::Utils::ForEachComponent(Registry, Entity, [&](void*, const ECS::FSparseSet&, CStruct* StructType)
        {
            FString Chip = LE_ICON_PUZZLE " ";
            Chip += StructType->MakeDisplayName().c_str();
            Display.TooltipChips.emplace_back(std::move(Chip));
        });
        if (Display.TooltipChips.empty())
        {
            Display.TooltipChips.emplace_back("(none)");
        }

        Display.bTooltipBuilt = true;
    }

    void FSceneEditorTool::RemoveEntityFromOutliner(ECS::FEntity Entity)
    {
        auto It = EntityToTreeNode.find(Entity);
        if (It == EntityToTreeNode.end())
        {
            return;
        }

        // RemoveNode tears down the subtree; walk hierarchy first to clear EntityToTreeNode for descendants.
        ECS::FRegistry& Registry = GetSceneRegistry();
        if (Registry.IsValid(Entity))
        {
            ECS::Utils::ForEachChild(Registry, Entity, [&](ECS::FEntity Child)
            {
                RemoveEntityFromOutliner(Child);
            });
        }

        OutlinerListView.RemoveNode(It->second);
        EntityToTreeNode.erase(It);
    }

    void FSceneEditorTool::ReparentEntityInOutliner(ECS::FEntity Entity)
    {
        // Remember the old tree parent before the move; it may lose its last child here.
        ECS::FEntity OldParent = ECS::NullEntity;
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

    void FSceneEditorTool::RefreshOutlinerExpander(ECS::FEntity Entity)
    {
        if (Entity == ECS::NullEntity)
        {
            return;
        }
        auto It = EntityToTreeNode.find(Entity);
        if (It == EntityToTreeNode.end())
        {
            return;
        }

        ECS::FRegistry& Registry = GetSceneRegistry();
        const FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Entity);
        const bool bHasChildren = Rel != nullptr && Rel->Children > 0;
        OutlinerListView.MarkHasLazyChildren(It->second, bHasChildren);
    }

    void FSceneEditorTool::RevealEntityInOutliner(ECS::FEntity Entity)
    {
        if (Entity == ECS::NullEntity)
        {
            return;
        }

        ECS::FRegistry& Registry = GetSceneRegistry();
        if (!Registry.IsValid(Entity) || !IsOutlinerEntityVisible(Entity))
        {
            return;
        }

        // Newly-spawned rows are queued; without this a select-on-spawn would find no node.
        FlushOutlinerPending();

        // Children are lazy, so each level has to be opened top-down before the next has a node.
        TVector<ECS::FEntity> Chain;
        for (ECS::FEntity Cursor = Entity; Cursor != ECS::NullEntity && Chain.size() < 64; )
        {
            Chain.push_back(Cursor);

            const FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Cursor);
            Cursor = (Rel != nullptr) ? Rel->Parent : ECS::NullEntity;
            if (Cursor != ECS::NullEntity && !Registry.IsValid(Cursor))
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
        ECS::FRegistry& Registry = GetSceneRegistry();
        if (!Registry.IsValid(Data.Entity))
        {
            return;
        }

        // Skips filtered-out children and ones already present from an on_construct race.
        ECS::Utils::ForEachChild(Registry, Data.Entity, [&](ECS::FEntity Child)
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

    void FSceneEditorTool::OnOutlinerEntityConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (Registry.HasAny<FHideInSceneOutliner>(Entity))
        {
            return;
        }
        // Defer to next flush; FRelationshipComponent may not be set yet.
        PendingOutlinerAdds.push_back(Entity);
    }

    void FSceneEditorTool::OnOutlinerEntityDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        (void)Registry;

        // Deleting the last child must drop the parent's twisty, and the parent link is still intact here.
        ECS::FEntity ParentEntity = ECS::NullEntity;
        if (auto It = EntityToTreeNode.find(Entity); It != EntityToTreeNode.end())
        {
            FTreeNodeID ParentNode = OutlinerListView.GetParentNode(It->second);
            if (ParentNode.IsValid())
            {
                ParentEntity = OutlinerListView.Get<FEntityListViewItemData>(ParentNode).Entity;
            }
        }
        else if (const FRelationshipComponent* Rel = GetSceneRegistry().TryGet<FRelationshipComponent>(Entity))
        {
            ParentEntity = Rel->Parent;
        }

        RemoveEntityFromOutliner(Entity);
        PendingOutlinerAdds.erase(Algo::Remove(PendingOutlinerAdds.begin(), PendingOutlinerAdds.end(), Entity), PendingOutlinerAdds.end());

        // Skipped during a restore, which recreates every entity and reloads the table from the snapshot.
        if (!bRestoringTransaction)
        {
            if (SSceneFolderComponent* Folders = GetEditableSceneFolders())
            {
                Folders->RemoveEntity(Entity);
            }
            EntityFolderCache.erase(Entity);
        }

        if (ParentEntity != ECS::NullEntity)
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

        // The destroy has settled by now, and the refresh no-ops for parents that were removed too.
        for (ECS::FEntity Parent : PendingExpanderRefresh)
        {
            RefreshOutlinerExpander(Parent);
        }
        PendingExpanderRefresh.clear();
    }

    namespace
    {
        FString MakeFolderPath(const SSceneFolderComponent& Folders, uint32 FolderID)
        {
            FString Path;
            uint32 Cursor = FolderID;
            for (uint32 Guard = 0; Cursor != SSceneFolderComponent::NoFolder && Guard < 64; ++Guard)
            {
                const FSceneFolder* Folder = Folders.Find(Cursor);
                if (Folder == nullptr)
                {
                    break;
                }

                Path = Path.empty() ? FString(Folder->Name.c_str()) : FString(Folder->Name.c_str()) + "/" + Path;
                Cursor = Folder->ParentID;
            }
            return Path;
        }

        int32 GetFolderDepth(const SSceneFolderComponent& Folders, uint32 FolderID)
        {
            int32 Depth = 0;
            const FSceneFolder* Folder = Folders.Find(FolderID);
            while (Folder != nullptr && Folder->ParentID != SSceneFolderComponent::NoFolder && Depth < 64)
            {
                Folder = Folders.Find(Folder->ParentID);
                ++Depth;
            }
            return Depth;
        }
    }

    const SSceneFolderComponent* FSceneEditorTool::GetSceneFolders() const
    {
        if (!SupportsSceneFolders() || GetObservedWorld() == nullptr)
        {
            return nullptr;
        }

        return GetObservedWorld()->FindSceneFolders();
    }

    SSceneFolderComponent* FSceneEditorTool::GetEditableSceneFolders() const
    {
        if (!SupportsSceneFolders() || IsInspectingForeignWorld() || World == nullptr)
        {
            return nullptr;
        }

        return World->FindSceneFolders();
    }

    uint32 FSceneEditorTool::GetEntityFolderID(ECS::FEntity Entity) const
    {
        auto It = EntityFolderCache.find(Entity);
        if (It != EntityFolderCache.end())
        {
            return It->second;
        }

        const SSceneFolderComponent* Folders = GetSceneFolders();
        return Folders != nullptr ? Folders->FindEntityFolder(Entity) : SSceneFolderComponent::NoFolder;
    }

    FTreeNodeID FSceneEditorTool::FindFolderNode(uint32 FolderID) const
    {
        auto It = FolderToTreeNode.find(FolderID);
        return It != FolderToTreeNode.end() ? It->second : InvalidTreeNode;
    }

    FString FSceneEditorTool::GetFolderPath(uint32 FolderID) const
    {
        const SSceneFolderComponent* Folders = GetSceneFolders();
        return Folders != nullptr ? MakeFolderPath(*Folders, FolderID) : FString();
    }

    FName FSceneEditorTool::MakeUniqueFolderName(uint32 ParentID) const
    {
        const SSceneFolderComponent* Folders = GetSceneFolders();
        if (Folders == nullptr)
        {
            return FName("New Folder");
        }

        for (int32 Suffix = 1; Suffix < 1000; ++Suffix)
        {
            FString Candidate = "New Folder";
            if (Suffix > 1)
            {
                Candidate += " ";
                Candidate += Format("{}", Suffix).c_str();
            }

            const FName CandidateName(Candidate.c_str());
            bool bTaken = false;
            for (const FSceneFolder& Folder : Folders->Folders)
            {
                if (Folder.ParentID == ParentID && Folder.Name == CandidateName)
                {
                    bTaken = true;
                    break;
                }
            }

            if (!bTaken)
            {
                return CandidateName;
            }
        }

        return FName("New Folder");
    }

    void FSceneEditorTool::BuildFolderNodes(FTreeListView& Tree)
    {
        FolderToTreeNode.clear();
        EntityFolderCache.clear();

        const SSceneFolderComponent* Folders = GetSceneFolders();
        if (Folders == nullptr || Folders->IsEmpty())
        {
            return;
        }

        ECS::FRegistry& Registry = GetSceneRegistry();

        // Dead handles are dropped here rather than on every read; a foreign world is only inspected.
        if (SSceneFolderComponent* Editable = GetEditableSceneFolders())
        {
            for (FSceneFolder& Folder : Editable->Folders)
            {
                for (int32 i = (int32)Folder.Entities.size() - 1; i >= 0; --i)
                {
                    if (!Registry.IsValid(static_cast<ECS::FEntity>(Folder.Entities[i])))
                    {
                        Folder.Entities.erase(Folder.Entities.begin() + i);
                    }
                }
            }
        }

        // Shallowest first, so a folder's parent row always exists by the time the folder is created.
        TVector<const FSceneFolder*> Ordered;
        Ordered.reserve(Folders->Folders.size());
        for (const FSceneFolder& Folder : Folders->Folders)
        {
            Ordered.push_back(&Folder);
        }

        Algo::Sort(Ordered.begin(), Ordered.end(), [Folders](const FSceneFolder* LHS, const FSceneFolder* RHS)
        {
            const int32 LHSDepth = GetFolderDepth(*Folders, LHS->ID);
            const int32 RHSDepth = GetFolderDepth(*Folders, RHS->ID);
            if (LHSDepth != RHSDepth)
            {
                return LHSDepth < RHSDepth;
            }
            if (LHS->Name != RHS->Name)
            {
                return LHS->Name < RHS->Name;
            }
            return LHS->ID < RHS->ID;
        });

        for (const FSceneFolder* Folder : Ordered)
        {
            FFixedString Label;
            Label.append(LE_ICON_FOLDER).append(" ").append(Folder->Name.c_str());

            // Hashed by id so a rebuild restores the folder's expansion state (entity rows carry no hash).
            constexpr uint64 FolderNodeHashSalt = 0xF01DE4ull << 32;

            const FTreeNodeID ParentNode = FindFolderNode(Folder->ParentID);
            const FTreeNodeID Node = Tree.CreateNode(ParentNode, FStringView(Label.data(), Label.length()), FolderNodeHashSalt | Folder->ID);
            FolderToTreeNode[Folder->ID] = Node;

            Tree.EmplaceUserData<FEntityListViewItemData>(Node).FolderID = Folder->ID;

            FTreeNodeDisplay& Display = Tree.Get<FTreeNodeDisplay>(Node);
            Display.IconText = LE_ICON_FOLDER;
            Display.IconColor = EditorColors::AccentAlt();
            Display.bAllowRenaming = true;
            Display.bShowDisabledIcon = true;
            Display.TooltipTitle = FString(LE_ICON_FOLDER " ") + Folder->Name.c_str();
            Display.TooltipSubtitle = "Outliner folder, organization only";
            Display.bTooltipBuilt = true;

            for (uint32 Handle : Folder->Entities)
            {
                const ECS::FEntity Member = static_cast<ECS::FEntity>(Handle);
                if (Registry.IsValid(Member))
                {
                    EntityFolderCache[Member] = Folder->ID;
                }
            }
        }

        // A folder reads as hidden only when everything inside it is.
        for (const FSceneFolder* Folder : Ordered)
        {
            TVector<ECS::FEntity> Contents;
            CollectFolderEntities(Folder->ID, Contents, true);
            if (Contents.empty())
            {
                continue;
            }

            bool bAllHidden = true;
            for (ECS::FEntity Member : Contents)
            {
                if (!Registry.HasAny<SDisabledTag>(Member))
                {
                    bAllHidden = false;
                    break;
                }
            }

            Tree.Get<FTreeNodeState>(FindFolderNode(Folder->ID)).bDisabled = bAllHidden;
        }
    }

    void FSceneEditorTool::CollectFolderEntities(uint32 FolderID, TVector<ECS::FEntity>& OutEntities, bool bRecursive) const
    {
        const SSceneFolderComponent* Folders = GetSceneFolders();
        if (Folders == nullptr)
        {
            return;
        }

        TVector<uint32> FolderIDs;
        FolderIDs.push_back(FolderID);
        if (bRecursive)
        {
            Folders->CollectDescendants(FolderID, FolderIDs);
        }

        ECS::FRegistry& Registry = GetSceneRegistry();
        for (uint32 ID : FolderIDs)
        {
            const FSceneFolder* Folder = Folders->Find(ID);
            if (Folder == nullptr)
            {
                continue;
            }

            for (uint32 Handle : Folder->Entities)
            {
                const ECS::FEntity Member = static_cast<ECS::FEntity>(Handle);
                if (Registry.IsValid(Member))
                {
                    OutEntities.push_back(Member);
                }
            }
        }
    }

    ECS::FEntity FSceneEditorTool::GetSceneFolderOwner() const
    {
        if (World == nullptr)
        {
            return ECS::NullEntity;
        }

        // The folder table lives on the world's singleton entity, which has no public accessor.
        for (ECS::FEntity Entity : ECS::GetWorldRegistry(*World).View<FSingletonEntityTag>())
        {
            return Entity;
        }

        return ECS::NullEntity;
    }

    void FSceneEditorTool::BeginFolderTransaction()
    {
        const ECS::FEntity Owner = GetSceneFolderOwner();
        if (Owner == ECS::NullEntity)
        {
            BeginTransaction();
            return;
        }

        TVector<ECS::FEntity> Targets;
        Targets.push_back(Owner);
        BeginComponentTransaction(Targets, SSceneFolderComponent::StaticStruct());
    }

    void FSceneEditorTool::RecordPrefabOverrideSnapshot(const TVector<ECS::FEntity>& Entities)
    {
        ECS::FRegistry& Registry = GetSceneRegistry();

        // The ledger sits on the instance root, an ancestor, so the whole chain up is recorded.
        TVector<ECS::FEntity> Chain;
        for (ECS::FEntity Entity : Entities)
        {
            ECS::FEntity Cursor = Entity;
            for (uint32 Guard = 0; Cursor != ECS::NullEntity && Registry.IsValid(Cursor) && Guard < 64; ++Guard)
            {
                Chain.AddUnique(Cursor);

                const FRelationshipComponent* Link = Registry.TryGet<FRelationshipComponent>(Cursor);
                Cursor = Link != nullptr ? Link->Parent : ECS::NullEntity;
            }
        }

        RecordComponentSnapshot(Chain, SPrefabOverrideComponent::StaticStruct());
    }

    void FSceneEditorTool::RecordSceneFolderSnapshot()
    {
        const ECS::FEntity Owner = GetSceneFolderOwner();
        if (Owner == ECS::NullEntity)
        {
            return;
        }

        TVector<ECS::FEntity> Targets;
        Targets.push_back(Owner);
        RecordComponentSnapshot(Targets, SSceneFolderComponent::StaticStruct());
    }

    uint32 FSceneEditorTool::CreateSceneFolder(const FName& Name, uint32 ParentID)
    {
        if (!SupportsSceneFolders() || IsInspectingForeignWorld() || World == nullptr)
        {
            return SSceneFolderComponent::NoFolder;
        }

        BeginFolderTransaction();
        const uint32 NewFolder = World->GetSceneFolders().CreateFolder(Name, ParentID);
        EndTransaction("Create Folder");

        OutlinerListView.MarkTreeDirty();
        MarkSceneDirty();
        return NewFolder;
    }

    void FSceneEditorTool::RenameSceneFolder(uint32 FolderID, FStringView NewName)
    {
        SSceneFolderComponent* Folders = GetEditableSceneFolders();
        if (Folders == nullptr || Folders->Find(FolderID) == nullptr || NewName.empty())
        {
            return;
        }

        BeginFolderTransaction();
        Folders->RenameFolder(FolderID, FName(FString(NewName.data(), NewName.size()).c_str()));
        EndTransaction("Rename Folder");

        MarkSceneDirty();
    }

    void FSceneEditorTool::DeleteSceneFolder(uint32 FolderID, bool bDeleteContents)
    {
        SSceneFolderComponent* Folders = GetEditableSceneFolders();
        if (Folders == nullptr || Folders->Find(FolderID) == nullptr)
        {
            return;
        }

        if (bDeleteContents)
        {
            TVector<ECS::FEntity> Contents;
            CollectFolderEntities(FolderID, Contents, true);
            for (ECS::FEntity Entity : Contents)
            {
                EntityDestroyRequests.push(Entity);
            }
        }

        TVector<uint32> Doomed;
        Doomed.push_back(FolderID);
        Folders->CollectDescendants(FolderID, Doomed);

        BeginFolderTransaction();
        // Deepest first, so each folder's survivors land in a parent that is still there.
        for (int32 i = (int32)Doomed.size() - 1; i >= 0; --i)
        {
            Folders->RemoveFolder(Doomed[i]);
        }
        EndTransaction("Delete Folder");

        OutlinerListView.MarkTreeDirty();
        MarkSceneDirty();
    }

    void FSceneEditorTool::MoveFolderIntoFolder(uint32 FolderID, uint32 NewParentID)
    {
        SSceneFolderComponent* Folders = GetEditableSceneFolders();
        if (Folders == nullptr)
        {
            return;
        }

        BeginFolderTransaction();
        if (!Folders->SetFolderParent(FolderID, NewParentID))
        {
            AbortTransaction();
            return;
        }
        EndTransaction("Move Folder");

        OutlinerListView.MarkTreeDirty();
        MarkSceneDirty();
    }

    void FSceneEditorTool::MoveEntitiesToFolder(const TVector<ECS::FEntity>& Entities, uint32 FolderID)
    {
        if (!SupportsSceneFolders() || IsInspectingForeignWorld())
        {
            return;
        }

        // A null table is fine when unfiling, since the move still detaches from the entity parent.
        SSceneFolderComponent* Folders = GetEditableSceneFolders();
        if (FolderID != SSceneFolderComponent::NoFolder && (Folders == nullptr || Folders->Find(FolderID) == nullptr))
        {
            return;
        }

        ECS::FRegistry& Registry = GetSceneRegistry();

        TVector<ECS::FEntity> Moved;
        Moved.reserve(Entities.size());

        // Filing a parented entity detaches it, so the links travel with the folder table.
        BeginRelationshipTransaction(Entities);
        RecordSceneFolderSnapshot();

        for (ECS::FEntity Entity : Entities)
        {
            if (!IsOutlinerEntityVisible(Entity))
            {
                continue;
            }

            const SPrefabInstanceComponent* PrefabInstance = Registry.TryGet<SPrefabInstanceComponent>(Entity);
            if (PrefabInstance != nullptr && !PrefabInstance->bIsRoot)
            {
                continue;
            }

            // Folders only hold unparented entities, so filing one detaches it from its entity parent.
            if (ECS::Utils::IsChild(Registry, Entity))
            {
                ECS::Utils::RemoveFromParent(Registry, Entity);
            }

            if (Folders != nullptr)
            {
                Folders->AssignEntity(Entity, FolderID);
            }

            if (FolderID == SSceneFolderComponent::NoFolder)
            {
                EntityFolderCache.erase(Entity);
            }
            else
            {
                EntityFolderCache[Entity] = FolderID;
            }

            Moved.push_back(Entity);
        }

        if (Moved.empty())
        {
            AbortTransaction();
            return;
        }
        EndTransaction("Move To Folder");

        for (ECS::FEntity Entity : Moved)
        {
            ReparentEntityInOutliner(Entity);
        }

        MarkSceneDirty();
    }

    void FSceneEditorTool::SelectFolderContents(uint32 FolderID)
    {
        TVector<ECS::FEntity> Contents;
        CollectFolderEntities(FolderID, Contents, true);

        ClearSelectedEntities();
        for (ECS::FEntity Entity : Contents)
        {
            AddSelectedEntity(Entity);
        }
    }

    void FSceneEditorTool::SetFolderContentsHidden(uint32 FolderID, bool bHidden)
    {
        TVector<ECS::FEntity> Contents;
        CollectFolderEntities(FolderID, Contents, true);
        if (Contents.empty())
        {
            return;
        }

        // A pending rebuild re-derives every row's state from the registry, so stale ids are left alone.
        const bool bRowsCurrent = !OutlinerListView.IsDirty();

        ECS::FRegistry& Registry = GetSceneRegistry();
        for (ECS::FEntity Entity : Contents)
        {
            // Recorded in the prefab ledger, or the next refresh prunes the tag and unhides the instance.
            if (bHidden)
            {
                Registry.EmplaceOrReplace<SDisabledTag>(Entity);
                CPrefab::NoteComponentAdded(Registry, Entity, SDisabledTag::StaticStruct());
            }
            else
            {
                Registry.Remove<SDisabledTag>(Entity);
                CPrefab::NoteComponentRemoved(Registry, Entity, SDisabledTag::StaticStruct());
            }

            auto It = EntityToTreeNode.find(Entity);
            if (bRowsCurrent && It != EntityToTreeNode.end() && OutlinerListView.IsValid(It->second))
            {
                OutlinerListView.Get<FTreeNodeState>(It->second).bDisabled = bHidden;
            }
        }

        if (const SSceneFolderComponent* Folders = GetSceneFolders(); Folders != nullptr && bRowsCurrent)
        {
            TVector<uint32> Subfolders;
            Folders->CollectDescendants(FolderID, Subfolders);
            for (uint32 Subfolder : Subfolders)
            {
                const FTreeNodeID Node = FindFolderNode(Subfolder);
                if (OutlinerListView.IsValid(Node))
                {
                    OutlinerListView.Get<FTreeNodeState>(Node).bDisabled = bHidden;
                }
            }
        }

        MarkSceneDirty();
    }

    void FSceneEditorTool::DrawFolderContextMenu(uint32 FolderID)
    {
        const SSceneFolderComponent* Folders = GetSceneFolders();
        const FSceneFolder* Folder = Folders != nullptr ? Folders->Find(FolderID) : nullptr;
        if (Folder == nullptr)
        {
            return;
        }

        TVector<ECS::FEntity> Contents;
        CollectFolderEntities(FolderID, Contents, true);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(8, 4));

        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::AccentAlt());
        ImGui::TextUnformatted(LE_ICON_FOLDER_OPEN);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(Folder->Name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu)", (size_t)Contents.size());

        ImGui::Separator();

        if (ImGui::MenuItem(LE_ICON_FOLDER_PLUS " New Subfolder"))
        {
            PendingFolderRename = CreateSceneFolder(MakeUniqueFolderName(FolderID), FolderID);
        }

        if (ImGui::MenuItem(LE_ICON_PENCIL " Rename", "F2"))
        {
            PendingFolderRename = FolderID;
        }

        ImGui::Separator();

        if (ImGui::MenuItem(LE_ICON_SELECT_GROUP " Select Contents", nullptr, false, !Contents.empty()))
        {
            SelectFolderContents(FolderID);
        }

        if (ImGui::MenuItem(LE_ICON_EYE_OFF " Hide Contents", nullptr, false, !Contents.empty()))
        {
            SetFolderContentsHidden(FolderID, true);
        }

        if (ImGui::MenuItem(LE_ICON_EYE " Show Contents", nullptr, false, !Contents.empty()))
        {
            SetFolderContentsHidden(FolderID, false);
        }

        if (ImGui::BeginMenu(LE_ICON_FOLDER_MOVE " Move Folder Into"))
        {
            if (ImGui::MenuItem(LE_ICON_HOME " Root", nullptr, false, Folder->ParentID != SSceneFolderComponent::NoFolder))
            {
                MoveFolderIntoFolder(FolderID, SSceneFolderComponent::NoFolder);
            }

            ImGui::Separator();

            for (const FSceneFolder& Candidate : Folders->Folders)
            {
                if (Candidate.ID == FolderID || Folders->IsDescendantOf(Candidate.ID, FolderID))
                {
                    continue;
                }

                const FString Label = FString(LE_ICON_FOLDER " ") + MakeFolderPath(*Folders, Candidate.ID);
                if (ImGui::MenuItem(Label.c_str(), nullptr, false, Candidate.ID != Folder->ParentID))
                {
                    MoveFolderIntoFolder(FolderID, Candidate.ID);
                }
            }

            ImGui::EndMenu();
        }

        ImGui::Separator();

        if (ImGui::MenuItem(LE_ICON_FOLDER_REMOVE " Delete Folder"))
        {
            DeleteSceneFolder(FolderID, false);
        }
        ImGuiX::TextTooltip("{}", "Removes the folder. Its contents move up to the parent folder.");

        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Danger());
        if (ImGui::MenuItem(LE_ICON_TRASH_CAN " Delete Folder and Contents"))
        {
            DeleteSceneFolder(FolderID, true);
        }
        ImGui::PopStyleColor();
        ImGuiX::TextTooltip("{}", "Deletes the folder and every entity inside it.");

        ImGui::PopStyleVar(3);
    }

    void FSceneEditorTool::DrawMoveToFolderMenuItems(const TVector<ECS::FEntity>& Entities)
    {
        if (!SupportsSceneFolders() || IsInspectingForeignWorld() || Entities.empty())
        {
            return;
        }

        if (!ImGui::BeginMenu(LE_ICON_FOLDER_MOVE " Move to Folder"))
        {
            return;
        }

        if (ImGui::MenuItem(LE_ICON_HOME " Root"))
        {
            MoveEntitiesToFolder(Entities, SSceneFolderComponent::NoFolder);
        }

        if (const SSceneFolderComponent* Folders = GetSceneFolders())
        {
            if (!Folders->IsEmpty())
            {
                ImGui::Separator();
            }

            for (const FSceneFolder& Folder : Folders->Folders)
            {
                const FString Label = FString(LE_ICON_FOLDER " ") + MakeFolderPath(*Folders, Folder.ID);
                if (ImGui::MenuItem(Label.c_str()))
                {
                    MoveEntitiesToFolder(Entities, Folder.ID);
                }
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem(LE_ICON_FOLDER_PLUS " New Folder..."))
        {
            const uint32 NewFolder = CreateSceneFolder(MakeUniqueFolderName(SSceneFolderComponent::NoFolder), SSceneFolderComponent::NoFolder);
            MoveEntitiesToFolder(Entities, NewFolder);
            PendingFolderRename = NewFolder;
        }

        ImGui::EndMenu();
    }

    void FSceneEditorTool::DrawNewFolderButton(float ButtonSize)
    {
        if (!SupportsSceneFolders())
        {
            return;
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(IsInspectingForeignWorld());
        if (ImGuiX::IconButton(LE_ICON_FOLDER_PLUS, "##NewSceneFolder", EditorColors::U32(EditorColors::AccentAlt()), ImVec2(ButtonSize, ButtonSize)))
        {
            PendingFolderRename = CreateSceneFolder(MakeUniqueFolderName(SSceneFolderComponent::NoFolder), SSceneFolderComponent::NoFolder);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("New folder. Folders group entities in the outliner only.");
        }
        ImGui::EndDisabled();
    }

    FTransform FSceneEditorTool::GetNewEntitySpawnTransform() const
    {
        return GetCameraSpawnTransform();
    }

    void FSceneEditorTool::RunTransacted(FName Label, const TFunction<void()>& Mutate)
    {
        if (!Mutate)
        {
            return;
        }

        BeginTransaction();
        Mutate();
        EndTransaction(Label);

        MarkSceneDirty();
        OutlinerListView.MarkTreeDirty();
        bDetailsDirty = true;
    }

    void FSceneEditorTool::RunCreationTransacted(FName Label, const TFunction<void()>& Mutate)
    {
        if (!Mutate)
        {
            return;
        }

        BeginCreationTransaction();
        Mutate();
        EndTransaction(Label);

        MarkSceneDirty();
        OutlinerListView.MarkTreeDirty();
        bDetailsDirty = true;
    }

    ECS::FEntity FSceneEditorTool::SpawnEntityTransacted(FName Name, FName TransactionLabel,
        const TFunction<void(ECS::FEntity)>& Decorate)
    {
        BeginCreationTransaction();

        const ECS::FEntity Created = World->ConstructEntity(Name, GetNewEntitySpawnTransform());
        if (Created != ECS::NullEntity)
        {
            if (Decorate)
            {
                Decorate(Created);
            }

            OnEntityCreatedInScene(Created);
        }

        EndTransaction(TransactionLabel);

        if (Created != ECS::NullEntity)
        {
            SetSingleSelectedEntity(Created);
            MarkSceneDirty();
        }

        return Created;
    }

    void FSceneEditorTool::CreateEntity()
    {
        SpawnEntityTransacted("Entity", "New Entity", nullptr);
    }

    void FSceneEditorTool::CreateEntityWithComponent(const CStruct* Component)
    {
        if (Component == nullptr)
        {
            return;
        }

        CStruct* ComponentType = SceneOps::ResolveComponentType(FStringView(Component->GetName().c_str()));
        const FComponentOps* Ops = ComponentType != nullptr ? ComponentType->GetComponentOps() : nullptr;
        if (Ops == nullptr)
        {
            return;
        }

        // A bare emplace, matching what this path has always done. It records no prefab override note.
        SpawnEntityTransacted(Component->MakeDisplayName(), "New Entity", [this, Ops](ECS::FEntity Created)
        {
            Ops->EmplaceDefault(GetSceneRegistry(), Created);
        });
    }

    void FSceneEditorTool::CreatePrimitiveEntity(CStaticMesh* PrimitiveMesh, const char* DisplayName)
    {
        if (PrimitiveMesh == nullptr)
        {
            return;
        }

        SpawnEntityTransacted(DisplayName, "New Primitive", [this, PrimitiveMesh](ECS::FEntity Created)
        {
            GetSceneRegistry().Emplace<SStaticMeshComponent>(Created).SetStaticMesh(PrimitiveMesh);
        });
    }

    TVector<ECS::FEntity> FSceneEditorTool::GetComponentEditTargets(ECS::FEntity Entity)
    {
        TVector<ECS::FEntity> Targets;

        ECS::FRegistry& Registry = GetSceneRegistry();
        if (!Registry.IsValid(Entity))
        {
            return Targets;
        }

        // Apply to the whole selection when more than one entity is selected; otherwise just the focus entity.
        if (SelectedEntities.size() > 1 && IsEntitySelected(Entity))
        {
            Targets.reserve(SelectedEntities.size());
            for (ECS::FEntity Selected : SelectedEntities)
            {
                if (Registry.IsValid(Selected))
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

    void FSceneEditorTool::ApplyAddComponentToTargets(const TVector<ECS::FEntity>& Targets, CStruct* PickedMetaType)
    {
        if (Targets.empty())
        {
            return;
        }

        ECS::FRegistry& Registry = GetSceneRegistry();

        // Planned before the transaction, because opening one snapshots the whole registry.
        const SceneOps::FAddComponentPlan Plan = SceneOps::PlanAddComponent(Registry, Targets, PickedMetaType);

        const char* ComponentName = Plan.Component ? Plan.Component->GetName().c_str() : "that component";

        if (!Plan.HasWork())
        {
            // Saying nothing is indistinguishable from the click not registering.
            if (Targets.size() == 1)
            {
                ImGuiX::Notifications::NotifyWarning("This entity already has {0}.", ComponentName);
            }
            else
            {
                ImGuiX::Notifications::NotifyWarning("All {0} selected entities already have {1}.",
                    (uint32)Targets.size(), ComponentName);
            }

            return;
        }

        BeginComponentTransaction(Plan.Missing, Plan.Component);
        RecordPrefabOverrideSnapshot(Plan.Missing);
        SceneOps::ApplyAddComponent(Registry, Plan, PickedMetaType);
        EndTransaction("Add Component");

        if (Plan.AlreadyPresent > 0)
        {
            ImGuiX::Notifications::NotifyInfo("Added {0} to {1} entities; {2} already had it.",
                Plan.Component ? Plan.Component->GetName().c_str() : "component",
                (uint32)Plan.Missing.size(), Plan.AlreadyPresent);
        }

        MarkSceneDirty();
        OutlinerListView.MarkTreeDirty();
        bDetailsDirty = true;
    }

    namespace
    {
        // Compact and purely vertical, so it scales to large component counts.
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

        // Force-opens while a filter is active, so matches stay visible in a collapsed section.
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
            Header.append(Format("{}", Count).c_str());
            Header.append(")");

            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::SectionHeader());
            const bool bOpen = ImGui::CollapsingHeader(Header.c_str(),
                ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
            ImGui::PopStyleColor();
            return bOpen;
        }

        // Expects to be called between BeginTable and EndTable on a two-column table.
        bool DrawPickerRow(const void* ID, const char* Icon, const char* Label)
        {
            ImGui::PushID(ID);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            const bool bClicked = ImGui::Selectable("##sel", false,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);

            ImGui::SameLine();
            // A soft slate tone keeps the icon column a rail rather than an accent stripe down a long list.
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::SectionHeader());
            ImGui::TextUnformatted(Icon);
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(Label);

            ImGui::PopID();
            return bClicked;
        }
    }

    bool FSceneEditorTool::DrawAddableComponentList(const ImGuiTextFilter& Filter, const TVector<ECS::FEntity>& Targets, CStruct*& OutMetaType, CStruct*& OutStruct)
    {
        struct FComponentEntry
        {
            CStruct* MetaType = nullptr;
            CStruct* Struct   = nullptr;   // reflected component
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

        ForEachComponentStruct([&](CStruct* Struct)
        {
            CStruct* MetaType = Struct;

            if (Struct->HasMeta("HideInComponentList"))
            {
                return;
            }

            FFixedString DisplayName = Struct->MakeDisplayName();
            if (!ImGuiX::PassSearchFilter(Filter, DisplayName.c_str()))
            {
                return;
            }

            FString CategoryName = Struct->HasMeta("Category")
                ? Struct->GetMeta("Category")
                : DefaultCategoryName;

            FComponentEntry NewEntry;
            NewEntry.MetaType = MetaType;
            NewEntry.Struct   = Struct;
            FindOrAddCategory(CategoryName).Entries.push_back(NewEntry);
        });

        Algo::Sort(Categories.begin(), Categories.end(), [](const FComponentCategory& LHS, const FComponentCategory& RHS)
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

        ECS::FRegistry& Registry = GetSceneRegistry();

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 3.0f));
        for (FComponentCategory& Category : Categories)
        {
            auto EntryName = [](const FComponentEntry& E) -> FString
            {
                return E.Struct->GetName().ToString();
            };
            Algo::Sort(Category.Entries.begin(), Category.Entries.end(), [&](const FComponentEntry& LHS, const FComponentEntry& RHS)
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
                        // The add skips targets that already hold it, so offering it would be offering a no-op.
                        bool bEveryTargetHasIt = !Targets.empty();
                        for (ECS::FEntity Target : Targets)
                        {
                            if (!ECS::Utils::HasComponent(Registry, Target, Entry.MetaType))
                            {
                                bEveryTargetHasIt = false;
                                break;
                            }
                        }

                        ImGui::BeginDisabled(bEveryTargetHasIt);

                        // The entry list is rebuilt every frame, and an unstable ID breaks press and release matching.
                        FFixedString DisplayName = Entry.Struct->MakeDisplayName();
                        if (DrawPickerRow((void*)Entry.Struct, CategoryIcon, DisplayName.c_str()))
                        {
                            OutMetaType = Entry.MetaType;
                            OutStruct   = Entry.Struct;
                            bPicked = true;
                        }

                        ImGui::EndDisabled();

                        // Outside the disabled scope, since a disabled item does not report hover.
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

    void FSceneEditorTool::AddEntityToCopies(ECS::FEntity Entity)
    {
        GetSceneRegistry().EmplaceOrReplace<FCopiedTag>(Entity);
    }

    void FSceneEditorTool::RemoveEntityFromCopies(ECS::FEntity Entity)
    {
        GetSceneRegistry().Remove<FCopiedTag>(Entity);
    }

    void FSceneEditorTool::ClearCopies() const
    {
        GetSceneRegistry().ClearComponent<FCopiedTag>();
    }

    void FSceneEditorTool::CopyEntity(ECS::FEntity& To, ECS::FEntity From)
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

            // Debug and view controls stay reachable mid-play, since the game-focused case returned earlier.
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

        // The editor only ever cycles between the three whole-transform modes.
        default:                             break;
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

        // A new scene can land at the freed one's address, so SetCaptureView's return is authoritative.
        if (RenderScene != CameraPreviewScene)
        {
            CameraPreviewScene  = RenderScene;
            CameraPreviewHandle = -1;
        }

        // A foreign observed world's selection has nothing to render, and a running game never previews.
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        const ECS::FEntity Selected = GetLastSelectedEntity();
        const bool bWantPreview =
            AllowCameraPreview() &&
            !World->IsGameWorld() && !IsInspectingForeignWorld() &&
            Registry.IsValid(Selected) &&
            Registry.HasAll<SCameraComponent, STransformComponent>(Selected);

        if (!bWantPreview)
        {
            if (CameraPreviewHandle >= 0 && !RenderScene->SetCaptureView(CameraPreviewHandle, FViewVolume{}, false))
            {
                CameraPreviewHandle = -1;   // stale handle from a rebuilt scene
            }
            return;
        }

        // A non-active camera has no resolved ViewVolume, so the view is built from its transform.
        const SCameraComponent& Camera = Registry.Get<SCameraComponent>(Selected);
        STransformComponent& Transform = Registry.Get<STransformComponent>(Selected);
        (void)Transform.GetWorldMatrix();   // ensure the world transform is current

        const FVector3 Position = Transform.GetWorldLocation();
        const FQuat Rotation = Transform.GetWorldRotation();
        const FVector3 Forward  = Rotation * FVector3(0.0f, 0.0f, 1.0f);
        const FVector3 Up       = Rotation * FVector3(0.0f, 1.0f, 0.0f);

        // ViewVolume only tracks the property for the active camera, so a selected one would be stale.
        FViewVolume View(Camera.FOV, (float)CameraPreviewWidth / (float)CameraPreviewHeight);
        View.SetView(Position, Forward, Up);

        // If the first call rejects the handle, register against the live scene and retry once.
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
        // Drag the top-left corner grip to resize, and the scale persists per tool.
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

        // Diagonal corner ticks, brightened while hovered or dragging.
        const ImU32 GripCol = bCameraPreviewMouseOver ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 140);
        DL->AddLine(ImVec2(Min.x + 2.0f, Min.y + 9.0f),  ImVec2(Min.x + 9.0f,  Min.y + 2.0f), GripCol, 2.0f);
        DL->AddLine(ImVec2(Min.x + 2.0f, Min.y + 14.0f), ImVec2(Min.x + 14.0f, Min.y + 2.0f), GripCol, 2.0f);
    }

    void FSceneEditorTool::EndFrame()
    {

        if (!bShowComponentVisualizers)
        {
            return;
        }

        CComponentVisualizerRegistry& ComponentVisualizerRegistry = CComponentVisualizerRegistry::Get();
        ECS::FRegistry& Registry = GetSceneRegistry();

        // Memoized because a type's visualizer is fixed once registered; a new registration drops the table.
        static thread_local THashMap<uint32, CComponentVisualizer*> VisualizerByType;
        static thread_local SIZE_T CachedVisualizerCount = ~(SIZE_T)0;

        const SIZE_T RegisteredVisualizers = ComponentVisualizerRegistry.GetVisualizers().size();
        if (CachedVisualizerCount != RegisteredVisualizers)
        {
            VisualizerByType.clear();
            CachedVisualizerCount = RegisteredVisualizers;
        }

        TFixedVector<TPair<ECS::FSparseSet*, CComponentVisualizer*>, 16> VisualizerStorages;
        for (Lumina::ECS::FSparseSet* StoragePtr : Registry.GetActiveStorages())
        {
            const Lumina::ECS::FComponentTypeID ID = StoragePtr->GetTypeInfo().TypeID;
            Lumina::ECS::FSparseSet& Storage = *StoragePtr;
            const uint32 TypeHash = (uint32)ID;

            CComponentVisualizer* Visualizer = nullptr;
            const auto Cached = VisualizerByType.find(TypeHash);
            if (Cached != VisualizerByType.end())
            {
                Visualizer = Cached->second;
            }
            else
            {
                if (CStruct* StructType = FindComponentStructByTypeId(ID))
                {
                    Visualizer = ComponentVisualizerRegistry.GetComponentVisualizer(StructType);
                }
                VisualizerByType[TypeHash] = Visualizer;
            }

            if (Visualizer != nullptr)
            {
                VisualizerStorages.emplace_back(&Storage, Visualizer);
            }
        }

        if (VisualizerStorages.empty())
        {
            return;
        }

        // Flattened from the view so the disabled-tag exclusion applies and workers can index it.
        static thread_local TVector<ECS::FEntity> SelectedList;
        SelectedList.clear();

        auto SelectedView = Registry.View<FSelectedInEditorComponent>(ECS::TExclude<SDisabledTag>{});
        SelectedList.reserve(SelectedView.Num());
        SelectedView.ForEach([&](ECS::FEntity SelectedEntity)
        {
            SelectedList.push_back(SelectedEntity);
        });

        if (SelectedList.empty())
        {
            return;
        }
        
        ECS::Utils::ResolveAllDirtyTransforms(Registry);

        auto DrawFor = [&](ECS::FEntity Entity)
        {
            for (auto& [Storage, Visualizer] : VisualizerStorages)
            {
                if (Storage->Contains(Entity))
                {
                    Visualizer->Draw(World, Registry, Entity);
                }
            }
        };
        
        Task::ParallelFor((uint32)SelectedList.size(), [&](uint32 Index)
        {
            const ECS::FEntity SelectedEntity = SelectedList[Index];
            DrawFor(SelectedEntity);
            ECS::Utils::ForEachChild(Registry, SelectedEntity, [&](ECS::FEntity Child)
            {
                DrawFor(Child);
            });
        }, 32);
    }

    void FSceneEditorTool::DrawDetailsPanel(bool bFocused)
    {
        const ECS::FEntity Entity = GetLastSelectedEntity();

        // PropertyTables hold raw component pointers; rebuild before drawing on focus change, invalidation, or dirty mark.
        const bool bEntityValid = (Entity != ECS::NullEntity) && GetSceneRegistry().IsValid(Entity);
        if (bEntityValid)
        {
            DrawComponentSearchBar();
        }

        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorColors::PanelBg());
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);

        ImGui::BeginChild("Property Editor", ImVec2(0, 0), true);

        // A C# reload or a prefab refresh frees what a built table points at, and nothing broadcasts it.
        const int32  ScriptGeneration = DotNet::GetScriptGeneration();
        const uint32 PrefabGeneration = CPrefab::GetDataGeneration();
        if (ScriptGeneration != DetailsScriptGeneration || PrefabGeneration != DetailsPrefabGeneration)
        {
            DetailsScriptGeneration = ScriptGeneration;
            DetailsPrefabGeneration = PrefabGeneration;

            // Dropped rather than rebuilt, since the old tables must not be touched again.
            PropertyTables.clear();
            DetailsEntity = ECS::NullEntity;
            bDetailsDirty = true;
        }

        if (!bEntityValid)
        {
            if (DetailsEntity != ECS::NullEntity || !PropertyTables.empty())
            {
                PropertyTables.clear();
                DetailsEntity = ECS::NullEntity;
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

    // On the base, since the prefab editor's preview world is what GetSceneRegistry resolves.
    void FSceneEditorTool::PushRenameEntityModal(ECS::FEntity Entity)
    {
        ToolContext->PushModal("Rename Entity", ImVec2(450.0f, 250.0f), [this, Entity]() -> bool
        {
            auto& NameComponent = GetSceneRegistry().Get<SNameComponent>(Entity);

            // Per modal rather than process-wide, so two rename dialogs cannot share a buffer.
            FFixedString& InputBuffer = RenameModalBuffer;

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

                // Without this the next prefab refresh would replace the component and revert the rename.
                CPrefab::RecaptureComponentOverrides(GetSceneRegistry(), Entity, SNameComponent::StaticStruct());

                // Update just this entity's row label rather than rebuilding the whole tree.
                auto It = EntityToTreeNode.find(Entity);
                if (It != EntityToTreeNode.end())
                {
                    FFixedString Label;
                    Label.append(LE_ICON_CUBE).append(" ")
                        .append(NameComponent.Name.c_str())
                        .append(FString(" - (" + Format("{}", (Entity).Value) + ")"));
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

    void FSceneEditorTool::DrawEntityProperties(ECS::FEntity Entity)
    {
        const bool bMultiSelect = SelectedEntities.size() > 1 && IsEntitySelected(Entity);

        SNameComponent* NameComponent = GetSceneRegistry().TryGet<SNameComponent>(Entity);
        FName EntityName = NameComponent ? NameComponent->Name : Format("{}", (uint32)Entity);

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
                ImGuiX::Text("ID {}", (Entity).Value);
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

            // Tool-specific header buttons, such as the world editor's Add Tag.
            DrawDetailsHeaderExtraButtons(Entity);

            ImGui::PopStyleColor(3);

            // One name, so a multi-selection rename has no meaning and an unnamed entity has no target.
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
                        for (ECS::FEntity Selected : SelectedEntities)
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

        // Tool-specific sections above the component list, such as the world editor's Tags.
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
        ECS::FRegistry& Registry = GetSceneRegistry();
        const auto NameStorage = Registry.GetStorage<SNameComponent>();
        
        size_t Count = NameStorage.GetDenseSize();
        for (ECS::FEntity Hidden : Registry.View<FHideInSceneOutliner>())
        {
            if (NameStorage.Contains(Hidden))
            {
                --Count;
            }
        }
        return Count;
    }

    void FSceneEditorTool::DrawFilterOptions()
    {

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

            for (Lumina::ECS::FSparseSet* StoragePtr : GetSceneRegistry().GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = StoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& Storage = *StoragePtr;
                if (CStruct* StructType = FindComponentStructByTypeId(ID))
                {
                    {
                        if (StructType->HasMeta("HideInComponentList"))
                        {
                            continue;
                        }

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();

                        auto It = Algo::Find(EntityFilterState.ComponentFilters.begin(),
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

            DrawNewFolderButton(ButtonWidth);

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

            // A plain square Button insets its text by FramePadding.x, too narrow for a glyph, so it left-aligns.
            if (ImGuiX::IconButton(LE_ICON_FILTER_SETTINGS, "##ComponentFilter", ImGui::GetColorU32(ImGuiCol_Text), ImVec2(ButtonWidth, ButtonWidth)))
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
            ImGui::Text(LE_ICON_FORMAT_LIST_NUMBERED " Total Entities: %zu", CountOutlinerEntities());
            const float RefreshButtonOffset = ImGui::GetContentRegionAvail().x - 24 - ImGui::GetStyle().FramePadding.x;
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextDisabled("(%zu)", (size_t)GetSceneRegistry().NumEntities());
            ImGui::SetItemTooltip("Entities listed below, parentheses include those hidden from the outliner.");
            ImGui::SameLine(RefreshButtonOffset);
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

                // A folder created this frame only gets its row on the rebuild the Draw above schedules.
                if (PendingFolderRename != SSceneFolderComponent::NoFolder && !OutlinerListView.IsDirty())
                {
                    const FTreeNodeID Node = FindFolderNode(PendingFolderRename);
                    if (Node.IsValid())
                    {
                        OutlinerListView.Get<FTreeNodeState>(Node).bEditingText = true;
                        OutlinerListView.RequestScrollToNode(Node);
                    }
                    PendingFolderRename = SSceneFolderComponent::NoFolder;
                }

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

    void FSceneEditorTool::DrawAddToEntityOrWorldPopup(ECS::FEntity Entity)
    {
        
        ImGui::SetNextWindowViewport(ImGui::GetWindowViewport()->ID);
        ImGui::SetNextWindowSize(ImVec2(450.0f, 550.0f), ImGuiCond_Always);

        if (ImGui::BeginPopup("AddToEntityMenu", ImGuiWindowFlags_NoMove))
        {
            if (Entity == ECS::NullEntity)
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

                if (Entity == ECS::NullEntity)
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
                            if (ImGuiX::PassSearchFilter(AddEntityComponentFilter, Data->AssetName.c_str()))
                            {
                                FilteredPrefabs.push_back(Data);
                            }
                        }

                        Algo::Sort(FilteredPrefabs.begin(), FilteredPrefabs.end(), [](FAssetData* LHS, FAssetData* RHS)
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
                                        HandlePrefabContentDrop(FStringView(Data->Path.c_str()), ECS::NullEntity, /*bAttachToTarget*/ false);
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
                        if (ImGuiX::PassSearchFilter(AddEntityComponentFilter, Entry.Name))
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
                                    if (GetSceneRegistry().IsValid(Entity))
                                    {
                                        if (PrimitiveMesh != nullptr)
                                        {
                                            BeginComponentTransaction({ Entity }, SStaticMeshComponent::StaticStruct());
                                            SStaticMeshComponent& MeshComp = GetSceneRegistry().EmplaceOrReplace<SStaticMeshComponent>(Entity);
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

                CStruct*              PickedMetaType = nullptr;
                CStruct*              PickedStruct = nullptr;

                // Resolved before the list is drawn so it can gray out what these targets already have.
                TVector<ECS::FEntity> Targets = GetComponentEditTargets(Entity);

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

                if (Entity == ECS::NullEntity)
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
