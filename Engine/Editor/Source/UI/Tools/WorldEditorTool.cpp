#include <string>
#include "World/ECS/Registry.h"
#include "WorldEditorTool.h"
#include "Core/CoreEditorDelegates.h"
#include "Play/StandaloneLauncher.h"
#include "Core/Math/Math.h"
#include "EditorToolContext.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Components/EditorEntityTags.h"
#include "ContentBrowserEditorTool.h"
#include "Config/Config.h"
#include "Core/Object/ObjectCore.h"
#include "Settings/EditorSettings.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Core/Application/Application.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Input/InputContext.h"
#include "Input/InputProcessor.h"
#include "UI/RmlUiBridge.h"
#include "Input/InputViewport.h"
#include "Memory/SmartPtr.h"
#include "Core/Math/Math.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Thumbnails/ThumbnailManager.h"
#include "Tools/ComponentVisualizers/ComponentVisualizer.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "Tools/Dialogs/Dialogs.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "World/Entity/Components/SceneFolderComponent.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "TerrainEditMode.h"
#include "FoliageEditMode.h"
#include "SequencerEditMode.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "UI/Tools/EditorEntityUtils.h"
#include "UI/Properties/PropertyEditContexts.h"
#include "World/WorldManager.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/TransformComponent.h"
#include <cmath>
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/SocketAttachmentComponent.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TagComponent.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "World/Subsystems/WorldSettings.h"
#include "Log/Log.h"
#include "Renderer/MeshQuantization.h"
#include "Containers/StringFormat.h"


namespace Lumina
{
    static constexpr const char* WorldSettingsName = "World Settings";
    static constexpr const char* SceneGraphName = "Scene Graph";
    static constexpr const char* SystemsName = "Systems";
    
    static FVector3 SanitizeManipulationScale(FVector3 Scale)
    {
        constexpr float MinScale = 0.001f;
        for (int i = 0; i < 3; ++i)
        {
            if (!std::isfinite(Scale[i]) || std::abs(Scale[i]) < MinScale)
            {
                Scale[i] = Scale[i] < 0.0f ? -MinScale : MinScale;
            }
        }
        return Scale;
    }

    static bool IsLockedPrefabChild(const ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
        {
            return false;
        }
        const SPrefabInstanceComponent* Instance = Registry.TryGet<SPrefabInstanceComponent>(Entity);
        return Instance != nullptr && !Instance->bIsRoot;
    }
    
    static ECS::FEntity ResolveSelectionRootForViewportPick(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
        {
            return Entity;
        }

        // The picked entity is itself a root, keep it.
        if (Registry.HasAll<FSelectionRoot>(Entity))
        {
            return Entity;
        }
        if (const SPrefabInstanceComponent* Instance = Registry.TryGet<SPrefabInstanceComponent>(Entity))
        {
            if (Instance->bIsRoot)
            {
                return Entity;
            }
        }

        const FRelationshipComponent* Relationship = Registry.TryGet<FRelationshipComponent>(Entity);
        while (Relationship != nullptr && Relationship->Parent != ECS::NullEntity)
        {
            ECS::FEntity Parent = Relationship->Parent;
            if (Registry.HasAll<FSelectionRoot>(Parent))
            {
                return Parent;
            }
            if (const SPrefabInstanceComponent* ParentInstance = Registry.TryGet<SPrefabInstanceComponent>(Parent))
            {
                if (ParentInstance->bIsRoot)
                {
                    return Parent;
                }
            }
            Relationship = Registry.TryGet<FRelationshipComponent>(Parent);
        }

        return Entity;
    }

    // The editor world has no physics scene, so mesh AABBs are projected in software.

    // Project world AABB to screen rect (y-down, viewport pixels). Returns false if the box is behind near plane.
    static bool ProjectAABBToScreenRect(const FAABB& WorldAABB, const FMatrix4& ViewProj,
                                        const ImVec2& ViewportSize,
                                        ImVec2& OutMin, ImVec2& OutMax)
    {
        const FVector3 Corners[8] = {
            { WorldAABB.Min.x, WorldAABB.Min.y, WorldAABB.Min.z },
            { WorldAABB.Max.x, WorldAABB.Min.y, WorldAABB.Min.z },
            { WorldAABB.Min.x, WorldAABB.Max.y, WorldAABB.Min.z },
            { WorldAABB.Max.x, WorldAABB.Max.y, WorldAABB.Min.z },
            { WorldAABB.Min.x, WorldAABB.Min.y, WorldAABB.Max.z },
            { WorldAABB.Max.x, WorldAABB.Min.y, WorldAABB.Max.z },
            { WorldAABB.Min.x, WorldAABB.Max.y, WorldAABB.Max.z },
            { WorldAABB.Max.x, WorldAABB.Max.y, WorldAABB.Max.z },
        };

        OutMin = ImVec2( FLT_MAX,  FLT_MAX);
        OutMax = ImVec2(-FLT_MAX, -FLT_MAX);
        bool bAnyInFront = false;

        for (const FVector3& Corner : Corners)
        {
            FVector4 Clip = ViewProj * FVector4(Corner, 1.0f);
            if (Clip.w <= 1e-4f)
            {
                continue;
            }
            const float NdcX = Clip.x / Clip.w;
            const float NdcY = Clip.y / Clip.w;
            // Caller flipped [1][1] to GL-Y-up; convert NDC +Y up to y-down pixels.
            const float Px = (NdcX * 0.5f + 0.5f) * ViewportSize.x;
            const float Py = (1.0f - (NdcY * 0.5f + 0.5f)) * ViewportSize.y;
            OutMin.x = Math::Min(OutMin.x, Px);
            OutMin.y = Math::Min(OutMin.y, Py);
            OutMax.x = Math::Max(OutMax.x, Px);
            OutMax.y = Math::Max(OutMax.y, Py);
            bAnyInFront = true;
        }

        return bAnyInFront;
    }

    // Slab ray-vs-AABB; OutT is along (possibly unnormalized) Dir. Returns true on hit in front of origin.
    static bool RayVsAABB(const FVector3& Origin, const FVector3& Dir,
                          const FAABB& Box, float& OutT)
    {
        float TMin = 0.0f;
        float TMax = FLT_MAX;
        for (int Axis = 0; Axis < 3; ++Axis)
        {
            if (Math::Abs(Dir[Axis]) < 1e-6f)
            {
                if (Origin[Axis] < Box.Min[Axis] || Origin[Axis] > Box.Max[Axis])
                {
                    return false;
                }
                continue;
            }
            float T1 = (Box.Min[Axis] - Origin[Axis]) / Dir[Axis];
            float T2 = (Box.Max[Axis] - Origin[Axis]) / Dir[Axis];
            if (T1 > T2) { std::swap(T1, T2); }
            TMin = Math::Max(TMin, T1);
            TMax = Math::Min(TMax, T2);
            if (TMin > TMax)
            {
                return false;
            }
        }
        OutT = TMin;
        return true;
    }

    // Project world point to viewport pixels (y-down); ViewProj uses GL-Y-up like ProjectAABBToScreenRect.
    static bool ProjectPointToScreen(const FVector3& WorldPos, const FMatrix4& ViewProj,
                                     const ImVec2& ViewportSize, ImVec2& OutScreen)
    {
        FVector4 Clip = ViewProj * FVector4(WorldPos, 1.0f);
        if (Clip.w <= 1e-4f)
        {
            return false;
        }
        const float NdcX = Clip.x / Clip.w;
        const float NdcY = Clip.y / Clip.w;
        OutScreen.x = (NdcX * 0.5f + 0.5f) * ViewportSize.x;
        OutScreen.y = (1.0f - (NdcY * 0.5f + 0.5f)) * ViewportSize.y;
        return true;
    }

    // Walk LOD-0 verts. Meshlet vertex positions are full mesh-local float3 (no quantization).
    template <typename TVisitor>
    static void ForEachMeshVertexLocal(const CStaticMesh& Mesh, TVisitor&& Visit)
    {
        const FMeshResource& Resource = Mesh.GetMeshResource();
        if (Resource.bSkinnedMesh)
        {
            return;
        }
        const FMeshletData& MeshletData = Resource.MeshletData;
        if (MeshletData.IsEmpty())
        {
            return;
        }
        const TVector<FMeshletVertex>& Verts = MeshletData.MeshletVertices;
        const TVector<FMeshlet>& Meshlets    = MeshletData.Meshlets;

        Mesh.ForEachSurface([&](const FGeometrySurface& Surface, uint32)
        {
            if (Surface.NumLODs == 0)
            {
                return;
            }
            const uint32 Offset = Surface.LODMeshletOffset[0];
            const uint32 Count  = Surface.LODMeshletCount[0];
            for (uint32 i = 0; i < Count; ++i)
            {
                const FMeshlet& M = Meshlets[Offset + i];
                for (uint32 v = 0; v < M.VertexCount; ++v)
                {
                    Visit(DecodeMeshletPosition(M, Verts[M.VertexOffset + v]));
                }
            }
        });
    }

    // LOD-0 vertex closest to TargetScreenPos; returns true if any projected within MaxScreenDistPx.
    static bool FindClosestVertexToScreenPoint(const CStaticMesh& Mesh,
                                               const FMatrix4& MeshWorldMatrix,
                                               const FMatrix4& ViewProj,
                                               const ImVec2& ViewportSize,
                                               const ImVec2& TargetScreenPos,
                                               float MaxScreenDistPx,
                                               FVector3& OutLocalPos,
                                               FVector3& OutWorldPos)
    {
        const FMeshResource& Resource = Mesh.GetMeshResource();
        if (Resource.bSkinnedMesh || Resource.MeshletData.IsEmpty())
        {
            return false;
        }

        // Cheap whole-mesh cull against an inflated screen rect.
        ImVec2 BoxMin, BoxMax;
        const FAABB WorldAABB = Mesh.GetAABB().ToWorld(MeshWorldMatrix);
        if (!ProjectAABBToScreenRect(WorldAABB, ViewProj, ViewportSize, BoxMin, BoxMax))
        {
            return false;
        }
        if (TargetScreenPos.x < BoxMin.x - MaxScreenDistPx || TargetScreenPos.x > BoxMax.x + MaxScreenDistPx ||
            TargetScreenPos.y < BoxMin.y - MaxScreenDistPx || TargetScreenPos.y > BoxMax.y + MaxScreenDistPx)
        {
            return false;
        }

        const FMatrix4 MVP = ViewProj * MeshWorldMatrix;
        float BestDistSq = MaxScreenDistPx * MaxScreenDistPx;
        bool  bFound = false;

        ForEachMeshVertexLocal(Mesh, [&](const FVector3& LocalPos)
        {
            FVector4 Clip = MVP * FVector4(LocalPos, 1.0f);
            if (Clip.w <= 1e-4f)
            {
                return;
            }
            const float Px = (Clip.x / Clip.w * 0.5f + 0.5f) * ViewportSize.x;
            const float Py = (1.0f - (Clip.y / Clip.w * 0.5f + 0.5f)) * ViewportSize.y;
            const float dx = Px - TargetScreenPos.x;
            const float dy = Py - TargetScreenPos.y;
            const float DistSq = dx * dx + dy * dy;
            if (DistSq < BestDistSq)
            {
                BestDistSq  = DistSq;
                OutLocalPos = LocalPos;
                OutWorldPos = FVector3(MeshWorldMatrix * FVector4(LocalPos, 1.0f));
                bFound = true;
            }
        });

        return bFound;
    }


    FWorldEditorTool::FWorldEditorTool(IEditorToolContext* Context, CWorld* InWorld)
        : FSceneEditorTool(Context, "World Editor", InWorld)
    {
        GuizmoOp = ImGuizmo::TRANSLATE;
        GuizmoMode = ImGuizmo::WORLD;
    }

    void FWorldEditorTool::OnInitialize()
    {
        CreateToolWindow(SceneGraphName, [&] (bool bFocused)
        {
            DrawOutliner(bFocused);
        });
        
        CreateToolWindow(WorldSettingsName, [&](bool bFocused)
        {
            DrawWorldSettings(bFocused);
        });

        CreateToolWindow(SystemsName, [&](bool bFocused)
        {
            DrawSystemsPanel(bFocused);
        });

        CreateToolWindow("Details", [&] (bool bFocused)
        {
            DrawDetailsPanel(bFocused);
        });
        
        const CWorldEditorSettings* Settings = GetDefault<CWorldEditorSettings>();
        bGuizmoSnapEnabled  = Settings->bGizmoSnapEnabled;
        GuizmoSnapTranslate = Settings->GizmoSnapTranslate;
        GuizmoSnapRotate    = Settings->GizmoSnapRotate;
        GuizmoSnapScale     = Settings->GizmoSnapScale;
        CameraPreviewScale  = Math::Clamp(Settings->CameraPreviewScale, 0.25f, 1.5f);

        RegisterEditorActions();
        RegisterEditorModes();

        WorldSettingsPropertyTable = MakeUnique<FPropertyTable>(&World->GetDefaultWorldSettings(), SDefaultWorldSettings::StaticStruct());
        
        OutlinerContext.bAllowRangeSelect = true;

        OutlinerContext.SetDragDropFunction = [this] (FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            if (Data.FolderID != 0)
            {
                DragDrop::SetSceneFolderPayload(World, Data.FolderID);
                return;
            }

            DragDrop::SetEntityPayload(World, Data.Entity);
        };

        OutlinerContext.ItemContextMenuFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            if (Data.FolderID != 0)
            {
                DrawFolderContextMenu(Data.FolderID);
                return;
            }

            ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
            const bool bLocked = IsLockedPrefabChild(Registry, Data.Entity);

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(8, 4));

            // A header of entity name and id, matching the viewport menu.
            {
                const SNameComponent* Name = Registry.TryGet<SNameComponent>(Data.Entity);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                ImGui::TextUnformatted(Name ? Name->Name.c_str() : "<unnamed>");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                ImGui::Text("#%u", (uint32)(Data.Entity).Value);
                ImGui::PopStyleColor();

                if (bLocked)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.40f, 1.0f));
                    ImGui::TextUnformatted(LE_ICON_LOCK " Locked (Prefab Instance)");
                    ImGui::PopStyleColor();
                    ImGuiX::TextTooltip("{}", "This entity belongs to a prefab instance. Edit the source prefab to change its hierarchy.");
                }
            }

            ImGui::Separator();

            // --- Components & scripts ---
            if (ImGui::MenuItem(LE_ICON_PUZZLE " Add Component..."))
            {
                PushAddComponentModal(Data.Entity);
            }
            ImGuiX::TextTooltip("{}", "Add a new component to the entity");

            DrawScriptAttachMenuItems(Data.Entity);

            ImGui::Separator();

            // --- Edit ---
            if (ImGui::MenuItem(LE_ICON_PENCIL " Rename"))
            {
                PushRenameEntityModal(Data.Entity);
            }

            if (!bLocked && ImGui::MenuItem(LE_ICON_CONTENT_DUPLICATE " Duplicate"))
            {
                BeginCreationTransaction();
                ECS::FEntity New = ECS::NullEntity;
                CopyEntity(New, Data.Entity);
                if (New != ECS::NullEntity)
                {
                    EndTransaction("Duplicate");
                }
                else
                {
                    AbortTransaction();
                }
            }

            if (ImGui::MenuItem(LE_ICON_IDENTIFIER " Copy Entity ID"))
            {
                ImGui::SetClipboardText(Format("{}", (Data.Entity).Value).c_str());
            }
            ImGuiX::TextTooltip("{}", "Copy entity identifier to platform clipboard");

            ImGui::Separator();

            // --- Hierarchy ---
            if (!bLocked && ECS::Utils::IsChild(Registry, Data.Entity))
            {
                if (ImGui::MenuItem(LE_ICON_ARROW_UP_BOLD " Unparent"))
                {
                    BeginRelationshipTransaction({ Data.Entity });
                    ECS::Utils::RemoveFromParent(Registry, Data.Entity);
                    EndTransaction("Unparent");
                    ReparentEntityInOutliner(Data.Entity);
                }

                // Attach to a socket on the parent's mesh (one click; adds/retargets the attachment component).
                FSocketEditContext SocketData;
                BuildSocketPickerData(Data.Entity, SocketData);
                const SSocketAttachmentComponent* Attachment = Registry.TryGet<SSocketAttachmentComponent>(Data.Entity);

                if (!SocketData.Sockets.empty() && ImGui::BeginMenu(LE_ICON_LINK " Attach to Socket"))
                {
                    for (const FName& Socket : SocketData.Sockets)
                    {
                        const bool bCurrent = Attachment != nullptr && Attachment->SocketName == Socket;
                        if (ImGui::MenuItem(Socket.c_str(), nullptr, bCurrent))
                        {
                            const FRelationshipComponent* Relationship = Registry.TryGet<FRelationshipComponent>(Data.Entity);
                            BeginRelationshipTransaction({ Data.Entity }, Relationship->Parent);
                            World->AttachEntityToSocket(Data.Entity, Relationship->Parent, Socket);
                            EndTransaction("Attach to Socket");
                            if (Data.Entity == DetailsEntity)
                            {
                                bDetailsDirty = true;
                            }
                        }
                    }
                    ImGui::EndMenu();
                }

                if (Attachment != nullptr && ImGui::MenuItem(LE_ICON_LINK_OFF " Clear Socket Attachment"))
                {
                    BeginComponentTransaction({ Data.Entity }, SSocketAttachmentComponent::StaticStruct());
                    Registry.Remove<SSocketAttachmentComponent>(Data.Entity);
                    EndTransaction("Clear Socket Attachment");
                    if (Data.Entity == DetailsEntity)
                    {
                        bDetailsDirty = true;
                    }
                }
            }

            if (!bLocked && ECS::Utils::IsParent(Registry, Data.Entity))
            {
                if (ImGui::MenuItem(LE_ICON_CALL_SPLIT " Detach Children"))
                {
                    // Snapshot child IDs before mutating relationships, then move each in the tree.
                    TFixedVector<ECS::FEntity, 20> Children;
                    ECS::Utils::ForEachChild(Registry, Data.Entity, [&](ECS::FEntity Child) { Children.push_back(Child); });
                    BeginRelationshipTransaction({ Data.Entity });
                    ECS::Utils::DetachImmediateChildren(Registry, Data.Entity);
                    EndTransaction("Detach Children");
                    for (ECS::FEntity Child : Children)
                    {
                        ReparentEntityInOutliner(Child);
                    }
                }
            }

            if (!bLocked)
            {
                DrawMoveToFolderMenuItems(GetComponentEditTargets(Data.Entity));

                if (ImGui::MenuItem(LE_ICON_FOLDER_PLUS " New Folder from Selection"))
                {
                    const TVector<ECS::FEntity> Targets = GetComponentEditTargets(Data.Entity);
                    const uint32 NewFolder = CreateSceneFolder(MakeUniqueFolderName(0), 0);
                    MoveEntitiesToFolder(Targets, NewFolder);
                    PendingFolderRename = NewFolder;
                }
                ImGuiX::TextTooltip("{}", "Groups the selection in the outliner. Folders never affect transforms.");

                ImGui::Separator();
            }

            if (!bLocked)
            {
                const bool bIsSelectionRoot = Registry.HasAll<FSelectionRoot>(Data.Entity);
                const char* RootLabel = bIsSelectionRoot
                    ? LE_ICON_TARGET " Unmark Selection Root"
                    : LE_ICON_TARGET " Mark as Selection Root";
                if (ImGui::MenuItem(RootLabel))
                {
                    if (bIsSelectionRoot)
                    {
                        Registry.Remove<FSelectionRoot>(Data.Entity);
                    }
                    else
                    {
                        Registry.Emplace<FSelectionRoot>(Data.Entity);
                    }
                }
                ImGuiX::TextTooltip("{}", "Viewport clicks on any descendant will resolve up to this entity. Outliner clicks still select directly.");
            }

            ImGui::Separator();

            if (!bLocked && ImGui::MenuItem(LE_ICON_PACKAGE_VARIANT " Create Prefab from Entity..."))
            {
                PushCreatePrefabModalForEntity(Data.Entity);
            }
            ImGuiX::TextTooltip("{}", "Save this entity (and its descendants) as a reusable prefab asset.");
            
            if (const SPrefabInstanceComponent* Instance = Registry.TryGet<SPrefabInstanceComponent>(Data.Entity);
                Instance != nullptr && Instance->bIsRoot)
            {
                if (ImGui::MenuItem(LE_ICON_SYNC " Resync from Prefab", nullptr, false, Instance->SourcePrefab != nullptr))
                {
                    ResyncPrefabInstance(Data.Entity);
                }
                ImGuiX::TextTooltip("{}", "Re-apply the source prefab to this instance now. Per-instance overrides and the placed transform are kept.");

                if (ImGui::MenuItem(LE_ICON_LINK_VARIANT_OFF " Detach from Prefab"))
                {
                    DetachPrefabInstance(Data.Entity);
                }
                ImGuiX::TextTooltip("{}", "Unlink this instance from its source prefab; the entities become plain and stop syncing.");
            }

            if (!bLocked)
            {
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                if (ImGui::MenuItem(LE_ICON_TRASH_CAN " Delete"))
                {
                    EntityDestroyRequests.push(Data.Entity);
                }
                ImGui::PopStyleColor();
            }

            ImGui::PopStyleVar(3);
        };
        
        OutlinerContext.VisibilityToggleFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            FTreeNodeState& State = Tree.Get<FTreeNodeState>(Item);

            if (Data.FolderID != 0)
            {
                SetFolderContentsHidden(Data.FolderID, State.bDisabled);
                return;
            }

            if (State.bDisabled)
            {
                ECS::GetWorldRegistry(*World).Emplace<SDisabledTag>(Data.Entity);
            }
            else
            {
                ECS::GetWorldRegistry(*World).Remove<SDisabledTag>(Data.Entity);
            }
        };

        OutlinerContext.SecondaryToggleFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            const FTreeNodeState& State = Tree.Get<FTreeNodeState>(Item);
            if (Data.Entity == ECS::NullEntity)
            {
                return;
            }

            // Tag presence stops the script ticking while leaving the entity itself active.
            if (State.bSecondaryToggled)
            {
                ECS::GetWorldRegistry(*World).EmplaceOrReplace<SScriptDisabledTag>(Data.Entity);
            }
            else
            {
                ECS::GetWorldRegistry(*World).Remove<SScriptDisabledTag>(Data.Entity);
            }

            if (World->GetPackage() != nullptr)
            {
                World->GetPackage()->MarkDirty();
            }
        };

        OutlinerContext.HoveredFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
            if (!Registry.IsValid(Data.Entity))
            {
                return;
            }

            // Tooltip building (component scan) is deferred from tree-build to first hover.
            BuildEntityTooltip(Data.Entity, Tree.Get<FTreeNodeDisplay>(Item));

            EditorEntityUtils::DrawEntityBounds(World, Data.Entity, FColor::White, 3.0f);
        };

        OutlinerContext.RebuildTreeFunction = [this](FTreeListView& Tree)
        {
            RebuildSceneOutliner(Tree);
        };

        OutlinerContext.BuildChildrenFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            BuildEntityChildren(Tree, Item);
        };

        OutlinerContext.RenameFunction = [this](FTreeListView& Tree, FTreeNodeID Item, FStringView NewName)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);

            if (Data.FolderID != 0)
            {
                FFixedString FolderLabel;
                FolderLabel.append(LE_ICON_FOLDER).append(" ").append(NewName);

                FTreeNodeDisplay& FolderDisplay = Tree.Get<FTreeNodeDisplay>(Item);
                FolderDisplay.DisplayName = FolderLabel;
                FolderDisplay.TooltipTitle = FString(LE_ICON_FOLDER " ") + FString(NewName.data(), NewName.size());

                RenameSceneFolder(Data.FolderID, NewName);
                return;
            }

            FFixedString Name;
            Name.append(LE_ICON_CUBE).append(" ")
                .append(NewName).append(Format(" - ({})", (Data.Entity).Value));

			Tree.Get<FTreeNodeDisplay>(Item).DisplayName = Name;

            SNameComponent& NameComponent = ECS::GetWorldRegistry(*World).Get<SNameComponent>(Data.Entity);
            NameComponent.Name = NewName;
		};
        
        OutlinerContext.ItemSelectedFunction = [this](FTreeListView& Tree, FTreeNodeID Item, bool bShouldClear)
        {
            // The selection mutators own writing bSelected, so set, tags and outliner rows stay in sync.
            if (!Item.IsValid())
            {
                if (bShouldClear)
                {
                    ClearSelectedEntities();
                }
                return;
            }

            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);

            // A folder is not an entity, so clicking one drops the entity selection rather than keeping it.
            if (Data.FolderID != 0)
            {
                if (bShouldClear)
                {
                    ClearSelectedEntities();
                }
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

        OutlinerContext.DragDropFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            if (Data.FolderID != 0)
            {
                HandleFolderDragDrop(Data.FolderID);
                return;
            }

            HandleEntityEditorDragDrop(Tree, Data.Entity);
        };

        OutlinerContext.ItemDoubleClickedFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            if (Data.FolderID != 0)
            {
                return;
            }

            FocusViewportToEntity(Data.Entity);
        };

        OutlinerContext.FilterFunction = [&](FTreeListView& Tree, FTreeNodeID Item)
        {
            
            const FTreeNodeDisplay& Display = Tree.Get<FTreeNodeDisplay>(Item);
            
            bool bPasses = ImGuiX::PassSearchFilter(EntityFilterState.FilterName, Display.DisplayName.c_str());

            // A folder carries no components, so any component filter necessarily excludes it.
            if (Tree.Get<FEntityListViewItemData>(Item).FolderID != 0)
            {
                return bPasses && EntityFilterState.ComponentFilters.empty();
            }

            for (const FName& ComponentFilter : EntityFilterState.ComponentFilters)
            {
                FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);

                ECS::FEntity Entity = Data.Entity;
                
                if (const FComponentOps* Ops = FindComponentOps(ComponentFilter.c_str()))
                {
                    if (Ops->Has(ECS::GetWorldRegistry(*World), Entity) == 0)
                    {
                        bPasses = false;
                    }
                }
            }
            
            return bPasses;
        };


        RebindRegistryObservers();

        WorldTraveledHandle = FCoreDelegates::OnWorldTraveled.AddMember(this, &FWorldEditorTool::OnWorldTraveled);
        GameQuitHandle = FCoreDelegates::OnGameQuitRequested.AddMember(this, &FWorldEditorTool::OnGameQuitRequested);
    }

    void FWorldEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        FCoreDelegates::OnWorldTraveled.Remove(WorldTraveledHandle);
        WorldTraveledHandle = FDelegateHandle{};
        FCoreDelegates::OnGameQuitRequested.Remove(GameQuitHandle);
        GameQuitHandle = FDelegateHandle{};

        if (bSimulatingWorld)
        {
            SetWorldNewSimulate(false);
        }

        if (bGamePreviewRunning)
        {
            // Closing the tool mid-play ends PIE too, so subscribers must not be left holding the world.
            FCoreEditorDelegates::OnPIEEnd.Broadcast(World);
            OnGamePreviewStopRequested.Broadcast();
        }
        
        UnbindRegistryObservers();
    }

    void FWorldEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        if (bGameQuitRequested && UpdateContext.GetUpdateStage() == EUpdateStage::FrameStart)
        {
            bGameQuitRequested = false;
            if (bGamePreviewRunning)
            {
                SetWorldPlayInEditor(false);
            }
        }
        
        if (ObservedWorld != nullptr && (GWorldManager == nullptr || GWorldManager->FindContext(ObservedWorld) == nullptr))
        {
            ObservedRegistry = nullptr; // its registry is gone; make UnbindRegistryObservers a no-op
            ObservedWorld = nullptr;
            PropertyTables.clear();
            SelectedEntities.clear();
            LastSelectedEntity = ECS::NullEntity;
            DetailsEntity = ECS::NullEntity;
            bDetailsDirty = true;
            OutlinerListView.ClearTree();
            OutlinerListView.MarkTreeDirty();
            EntityToTreeNode.clear();
            PendingOutlinerAdds.clear();
            PendingExpanderRefresh.clear();
            RebindRegistryObservers();      // now binds to World
            ResyncSelectionFromRegistry();
        }

        FSceneEditorTool::Update(UpdateContext);
        
        TickEjectState();

        if (bStopPlayRequested)
        {
            bStopPlayRequested = false;
            if (bGamePreviewRunning)
            {
                SetWorldPlayInEditor(false);
            }
        }

        // Drive the selected-camera preview before the world extracts this frame.
        UpdateCameraPreview();

        DrawWorldGrid();

        ProcessComponentEditRequests();

        if (!EntityDestroyRequests.empty())
        {
            // Drained to a list first, since the command has to read them while they are still alive.
            TVector<ECS::FEntity> Doomed;
            while (!EntityDestroyRequests.empty())
            {
                Doomed.push_back(EntityDestroyRequests.front());
                EntityDestroyRequests.pop();
            }

            // Recorded once, so a Delete that queues several entities is one undo step.
            BeginDestroyTransaction(Doomed);
            bool bDestroyed = false;
            for (ECS::FEntity Entity : Doomed)
            {
                if (!ECS::GetWorldRegistry(*World).IsValid(Entity))
                {
                    LOG_WARN("Attempted to delete an invalid entity! {}", (Entity).Value);
                    continue;
                }

                World->DestroyEntity(Entity);
                bDestroyed = true;
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

        if (bSelectionTransformRefreshPending)
        {
            bSelectionTransformRefreshPending = false;
            for (ECS::FEntity Selected : SelectedEntities)
            {
                if (ECS::GetWorldRegistry(*World).IsValid(Selected))
                {
                    ECS::GetWorldRegistry(*World).EmplaceOrReplace<FNeedsTransformUpdate>(Selected);
                }
            }
        }

        // Once Ctrl has served as a chord it must not also mean snap, or a duplicate yanks onto a vertex.
        if (!ImGui::GetIO().KeyCtrl)
        {
            bCtrlConsumedByChord = false;
        }
        else if (!bCtrlConsumedByChord)
        {
            for (int32 Key = ImGuiKey_NamedKey_BEGIN; Key < ImGuiKey_NamedKey_END; ++Key)
            {
                const ImGuiKey Typed = (ImGuiKey)Key;

                // Ctrl and click IS the vertex-snap drag, and holding a modifier is not yet a command.
                const bool bIgnored = (Typed >= ImGuiKey_Mouse_BEGIN && Typed < ImGuiKey_Mouse_END)
                                   || Typed == ImGuiKey_LeftCtrl  || Typed == ImGuiKey_RightCtrl
                                   || Typed == ImGuiKey_LeftShift || Typed == ImGuiKey_RightShift
                                   || Typed == ImGuiKey_LeftAlt   || Typed == ImGuiKey_RightAlt
                                   || Typed == ImGuiKey_LeftSuper || Typed == ImGuiKey_RightSuper;

                if (!bIgnored && ImGui::IsKeyPressed(Typed, false))
                {
                    bCtrlConsumedByChord = true;
                    break;
                }
            }
        }

        // Gated off text input, so renaming an entity does not delete it.
        // Alt excluded so a Ctrl+Alt chord cannot also fire the plain Ctrl clipboard commands.
        const bool bSelectionEditActive = (bViewportHovered || bOutlinerActive)
            && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyAlt;

        if (bSelectionEditActive)
        {
            bool bCopyPressed = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_C);
            bool bDuplicatePressed = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_D);
            bool bDeletePressed = ImGui::IsKeyPressed(ImGuiKey_Delete) && !HasVisualizerSubSelection();

            if (bCopyPressed)
            {
                ClearCopies();
            }

            // Snapshot before mutating; iterating the view while emitting/destroying invalidates iterators.
            TFixedVector<ECS::FEntity, 64> CurrentSelection;
            CurrentSelection.reserve(SelectedEntities.size());
            for (ECS::FEntity Selected : SelectedEntities)
            {
                if (ECS::GetWorldRegistry(*World).IsValid(Selected))
                {
                    CurrentSelection.push_back(Selected);
                }
            }

            TFixedVector<ECS::FEntity, 64> NewlyDuplicated;

            // Snapshot once so a Ctrl+D batch is a single undo.
            const bool bWantDuplicateTransaction = bDuplicatePressed;
            if (bWantDuplicateTransaction)
            {
                BeginCreationTransaction();
            }

            for (ECS::FEntity SelectedEntity : CurrentSelection)
            {
                const bool bLocked = IsLockedPrefabChild(ECS::GetWorldRegistry(*World), SelectedEntity);

                if (bCopyPressed)
                {
                    AddEntityToCopies(SelectedEntity);
                }

                if (bDuplicatePressed && !bLocked)
                {
                    ECS::FEntity New = ECS::NullEntity;
                    CopyEntity(New, SelectedEntity);
                    if (New != ECS::NullEntity)
                    {
                        NewlyDuplicated.push_back(New);
                    }
                }

                if (bDeletePressed && !bLocked)
                {
                    EntityDestroyRequests.push(SelectedEntity);
                }
            }

            // Select the duplicates so Ctrl+D twice keeps moving the new copies.
            if (bDuplicatePressed && !NewlyDuplicated.empty())
            {
                ClearSelectedEntities();
                for (ECS::FEntity New : NewlyDuplicated)
                {
                    AddSelectedEntity(New, false);
                }
            }

            if (bWantDuplicateTransaction)
            {
                if (!NewlyDuplicated.empty())
                {
                    EndTransaction("Duplicate");
                }
                else
                {
                    AbortTransaction();
                }
            }
        }
        
        constexpr size_t kMaxIndividualSelectionBoxes = 256;

        if (!bGameViewMode && SelectedEntities.size() <= kMaxIndividualSelectionBoxes)
        {
            for (ECS::FEntity Entity : SelectedEntities)
            {
                if (!ECS::GetWorldRegistry(*World).IsValid(Entity))
                {
                    continue;
                }

                // Every selectable type gets the same box, with a unit-box fallback resolved by the shared helper.
                EditorEntityUtils::DrawEntitySelectionBox(World, Entity, FColor::Green, 0.2f, 5.0f);
            }
        }

        const bool bPastePressed = bSelectionEditActive
            && ImGui::IsKeyDown(ImGuiKey_LeftCtrl)
            && ImGui::IsKeyPressed(ImGuiKey_V, false);

        if (bPastePressed)
        {
            // Snapshot sources first; CopyEntity adds rows the view would otherwise re-paste.
            TFixedVector<ECS::FEntity, 64> CopySources;
            ECS::GetWorldRegistry(*World).View<FCopiedTag>().ForEach([&](ECS::FEntity Entity)
            {
                if (!IsLockedPrefabChild(ECS::GetWorldRegistry(*World), Entity))
                {
                    CopySources.push_back(Entity);
                }
            });

            if (!CopySources.empty())
            {
                BeginCreationTransaction();

                TFixedVector<ECS::FEntity, 64> NewlyPasted;
                for (ECS::FEntity Source : CopySources)
                {
                    ECS::FEntity New = ECS::NullEntity;
                    CopyEntity(New, Source);
                    if (New != ECS::NullEntity)
                    {
                        NewlyPasted.push_back(New);
                    }
                }

                if (!NewlyPasted.empty())
                {
                    ClearSelectedEntities();
                    for (ECS::FEntity New : NewlyPasted)
                    {
                        AddSelectedEntity(New, false);
                    }
                    EndTransaction("Paste");
                }
                else
                {
                    AbortTransaction();
                }
            }
        }
        
        // Loop-driven, so the bookmark chords are handled inline rather than as nine actions.
        if (bViewportHovered && !ImGui::GetIO().WantTextInput)
        {
            const ImGuiIO& IO = ImGui::GetIO();
            const bool bPlain = !IO.KeyCtrl && !IO.KeyShift && !IO.KeyAlt;

            for (int32 Slot = 0; Slot < NumCameraBookmarks; ++Slot)
            {
                const ImGuiKey TopKey = (ImGuiKey)((int)ImGuiKey_1 + Slot);
                const ImGuiKey PadKey = (ImGuiKey)((int)ImGuiKey_Keypad1 + Slot);
                const bool bPressed = ImGui::IsKeyPressed(TopKey, false) || ImGui::IsKeyPressed(PadKey, false);
                if (!bPressed)
                {
                    continue;
                }

                if (IO.KeyCtrl && !IO.KeyShift && !IO.KeyAlt)
                {
                    SaveCameraBookmark(Slot);
                }
                else if (bPlain)
                {
                    RecallCameraBookmark(Slot);
                }
            }
        }
    }

    void FWorldEditorTool::ToggleGameViewMode()
    {
        FSceneRenderSettings* Settings = nullptr;
        if (IRenderScene* RenderScene = World ? World->GetRenderer() : nullptr)
        {
            Settings = &RenderScene->GetSceneRenderSettings();
        }

        if (!bGameViewMode)
        {
            bSavedWorldGridEnabled = bWorldGridEnabled;
            bSavedShowComponentVisualizers = bShowComponentVisualizers;
            if (Settings)
            {
                bSavedDrawBillboards = Settings->bDrawBillboards;
                bSavedDrawAABB = Settings->bDrawAABB;
            }

            bWorldGridEnabled = false;
            bShowComponentVisualizers = false;
            if (Settings)
            {
                Settings->bDrawBillboards = false;
                Settings->bDrawAABB = false;
            }

            bGameViewMode = true;
        }
        else
        {
            bWorldGridEnabled = bSavedWorldGridEnabled;
            bShowComponentVisualizers = bSavedShowComponentVisualizers;
            if (Settings)
            {
                Settings->bDrawBillboards = bSavedDrawBillboards;
                Settings->bDrawAABB = bSavedDrawAABB;
            }

            bGameViewMode = false;
        }
    }

    void FWorldEditorTool::RegisterEditorActions()
    {
        // Hotkeys are suppressed while right-mouse flying so W/E/R/Q act as fly keys, not mode switches.
        auto Hovered      = [this]() { return bViewportHovered && !CameraState.bWasLooking; };
        auto EditorWorld  = [this]() { return World && World->GetWorldType() == EWorldType::Editor; };

        RegisterAction({"Translate Mode", "Gizmo", "Switch the gizmo to translate (move) mode",
            FInputChord{ImGuiKey_W}, [this]
            {
                GuizmoOp = ImGuizmo::TRANSLATE;
            }, Hovered});

        RegisterAction({"Rotate Mode", "Gizmo", "Switch the gizmo to rotate mode",
            FInputChord{ImGuiKey_E}, [this]
            {
                GuizmoOp = ImGuizmo::ROTATE;
            }, Hovered});

        RegisterAction({"Scale Mode", "Gizmo", "Switch the gizmo to scale mode",
            FInputChord{ImGuiKey_R}, [this]
            {
                GuizmoOp = ImGuizmo::SCALE;
            }, Hovered});

        RegisterAction({"Toggle Local/World", "Gizmo", "Switch the gizmo between world-space and entity-local space",
            FInputChord{ImGuiKey_X}, [this]
            {
                ToggleGuizmoMode();
            }, Hovered});

        RegisterAction({"Focus Selection", "View", "Frame the camera on the last-selected entity",
            FInputChord{ImGuiKey_F}, [this]
            {
                FocusViewportToEntity(GetLastSelectedEntity());
            }});

        RegisterAction({"Toggle Game View", "View", "Hide editor overlays so the viewport shows what a runtime camera would",
            FInputChord{ImGuiKey_G}, [this]
            {
                ToggleGameViewMode();
            }, Hovered});

        RegisterAction({"Frame All", "View", "Frame the camera on every entity in the world",
            FInputChord{ImGuiKey_Home}, [this]
            {
                FrameAllEntities();
            }, Hovered});

        RegisterAction({"Group Selected", "Selection", "Wrap the selection under a new parent entity",
            FInputChord{ImGuiKey_G, /*Ctrl*/true}, [this]
            {
                GroupSelectedEntities();
            }, Hovered});

        RegisterAction({"Drop to Floor", "Selection", "Project the selection straight down onto the nearest mesh",
            FInputChord{ImGuiKey_End}, [this]
            {
                DropSelectionToFloor(); 
            }, Hovered});

        RegisterAction({"Align Transform with View", "Transform", "Move and rotate the selection onto the editor camera",
            FInputChord{ImGuiKey_M, /*Ctrl*/true, /*Shift*/false, /*Alt*/true}, [this]
            {
                AlignSelectionToView(/*bRotationOnly*/false);
            }, Hovered});

        RegisterAction({"Align Rotation with View", "Transform", "Rotate the selection onto the editor camera, leaving position alone",
            FInputChord{ImGuiKey_F, /*Ctrl*/true, /*Shift*/false, /*Alt*/true}, [this]
            {
                AlignSelectionToView(/*bRotationOnly*/true);
            }, Hovered});

        RegisterAction({"Transform Dialog", "Transform", "Apply a numeric translate, rotate and scale to the selection",
            FInputChord{}, [this]
            {
                OpenTransformDialog();
            }});

        RegisterAction({"Align View to Transform", "Transform", "Move the editor camera onto the last-selected entity",
            FInputChord{ImGuiKey_V, /*Ctrl*/true, /*Shift*/false, /*Alt*/true}, [this]
            {
                AlignViewToSelection();
            }, Hovered});

        RegisterAction({"Copy Transform", "Selection", "Copy the last-selected entity's transform to the clipboard",
            FInputChord{ImGuiKey_C, true, true}, [this]
            {
                CopyTransformFromLastSelected();
            }, Hovered});

        RegisterAction({"Paste Transform", "Selection", "Apply the previously-copied transform to every selected entity",
            FInputChord{ImGuiKey_V, true, true}, [this]
            {
                PasteTransformToSelection();
            }, Hovered});

        RegisterAction({"Undo", "History", "Revert the last transacted edit",
            FInputChord{ImGuiKey_Z, true}, [this]
            {
                Undo();
            }, EditorWorld});

        RegisterAction({"Redo", "History", "Re-apply the last undone edit",
            FInputChord{ImGuiKey_Y, true}, [this]
            {
                Redo();
            }, EditorWorld});

        // FEditorUI routes Ctrl+S to the focused tool, so a live callback here would save twice.
        RegisterAction({"Save World", "File", "Save the current world",
            FInputChord{ImGuiKey_S, true}, nullptr});

        // Advisory entries, inline-handled shortcuts registered so the shortcuts window surfaces them.
        RegisterAction({"Copy Entities", "Selection", "Copy the selection to the entity clipboard",
            FInputChord{ImGuiKey_C, true}, nullptr});
        
        RegisterAction({"Duplicate Entities", "Selection", "Duplicate the selection in place",
            FInputChord{ImGuiKey_D, true}, nullptr});
        
        RegisterAction({"Paste Entities", "Selection", "Paste previously-copied entities",
            FInputChord{ImGuiKey_V, true}, nullptr});
        
        RegisterAction({"Delete Selection", "Selection", "Delete every selected entity",
            FInputChord{ImGuiKey_Delete}, nullptr});
        
        RegisterAction({"Recall Camera Bookmark", "Camera", "Press 1-9 to recall a saved camera position",
            FInputChord{}, nullptr});
        
        RegisterAction({"Save Camera Bookmark", "Camera", "Ctrl+1..9 saves the camera into the matching slot",
            FInputChord{}, nullptr});
    }

    void FWorldEditorTool::RegisterEditorModes()
    {
        // Selection must be first so ActiveModeIndex=0 matches the pre-existing default.
        EditorModes.clear();
        EditorModes.push_back(MakeUnique<FSelectionEditorMode>());
        EditorModes.push_back(MakeUnique<FTerrainEditMode>());
        EditorModes.push_back(MakeUnique<FFoliageEditMode>());
        EditorModes.push_back(MakeUnique<FNavigationEditMode>());
        EditorModes.push_back(MakeUnique<FSequencerEditMode>());

        // Modes call back into the host for editor services (e.g. undo transactions).
        for (TUniquePtr<IWorldEditorMode>& Mode : EditorModes)
        {
            Mode->SetContext(this);
        }

        ActiveModeIndex = 0;
        if (IWorldEditorMode* Active = GetActiveMode())
        {
            Active->OnEnter(World);
        }
    }

    void FWorldEditorTool::UpdateSimulationGrab(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize, bool bInViewportHovered)
    {
        (void)ViewportOrigin;
        (void)ViewportSize;

        constexpr uint32 InvalidBody = 0xFFFFFFFFu;
        constexpr float  GrabReach = 500.0f;

        // A spring is second order and rings, while asking for a velocity is first order and cannot overshoot.
        constexpr float  ApproachGain = 5.0f;
        constexpr float  MaxGrabSpeed = 6.0f;

        // How hard the body is pushed to reach the velocity it was asked for. Also 1/s.
        constexpr float  VelocityGain = 8.0f;
        constexpr float  MaxGrabAcceleration = 120.0f;

        // On a paused editor world a force does nothing, so the gesture would look broken rather than inert.
        Physics::IPhysicsScene* Scene = HasSimulatingWorld() && World != nullptr ? World->GetPhysicsScene() : nullptr;
        if (Scene == nullptr)
        {
            GrabbedBodyID = InvalidBody;
            return;
        }

        const bool bGrabHeld = ImGui::GetIO().KeyShift && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (GrabbedBodyID != InvalidBody && !bGrabHeld)
        {
            GrabbedBodyID = InvalidBody;
        }

        FVector3 RayOrigin, RayDirection;
        if (!BuildViewportRay(ImGui::GetMousePos(), RayOrigin, RayDirection))
        {
            return;
        }

        if (GrabbedBodyID == InvalidBody)
        {
            if (!bInViewportHovered || !ImGui::GetIO().KeyShift || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                return;
            }

            SRayCastSettings Settings;
            Settings.Start = RayOrigin;
            Settings.End = RayOrigin + RayDirection * GrabReach;

            const TOptional<SRayResult> Hit = Scene->CastRay(Settings);
            if (!Hit.has_value())
            {
                return;
            }

            // A static or kinematic body cannot answer a force, so latching on would pull on something immovable.
            const ECS::FEntity HitEntity = (ECS::FEntity)Hit->Entity;
            const SRigidBodyComponent* RigidBody = World->IsValidEntity(HitEntity)
                ? World->TryGetComponent<SRigidBodyComponent>(HitEntity) : nullptr;

            if (RigidBody == nullptr || RigidBody->BodyType != EBodyType::Dynamic)
            {
                return;
            }

            GrabbedBodyID = (uint32)Hit->BodyID;
            GrabDistance = Math::Length(Hit->Location - RayOrigin);
            GrabLocalOffset = Math::Inverse(Scene->GetBodyRotation(GrabbedBodyID))
                            * (Hit->Location - Scene->GetBodyPosition(GrabbedBodyID));
            return;
        }

        const FVector3 Target = RayOrigin + RayDirection * GrabDistance;
        const FVector3 AttachPoint = Scene->GetBodyPosition(GrabbedBodyID)
                                   + Scene->GetBodyRotation(GrabbedBodyID) * GrabLocalOffset;

        // Capped so a fast cursor asks for a drag rather than a launch, which stops the body arriving instantly.
        FVector3 DesiredVelocity = (Target - AttachPoint) * ApproachGain;

        const float DesiredSpeed = Math::Length(DesiredVelocity);
        if (DesiredSpeed > MaxGrabSpeed)
        {
            DesiredVelocity *= MaxGrabSpeed / DesiredSpeed;
        }

        // GetVelocityAtPoint includes spin, so a body rotating under the grab is corrected by the same term.
        FVector3 Acceleration = (DesiredVelocity - Scene->GetVelocityAtPoint(GrabbedBodyID, AttachPoint))
                              * VelocityGain;

        const float AccelerationMagnitude = Math::Length(Acceleration);
        if (AccelerationMagnitude > MaxGrabAcceleration)
        {
            Acceleration *= MaxGrabAcceleration / AccelerationMagnitude;
        }

        // A body reporting zero mass is static or kinematic, so the grab lets go rather than pulling on nothing.
        const float BodyMass = Scene->GetBodyMass(GrabbedBodyID);
        if (BodyMass <= 0.0f)
        {
            GrabbedBodyID = InvalidBody;
            return;
        }

        SAddForceAtPositionEvent ForceEvent;
        ForceEvent.BodyID = GrabbedBodyID;
        ForceEvent.Force = Acceleration * BodyMass;
        ForceEvent.Position = AttachPoint;

        Scene->ActivateBody(GrabbedBodyID);
        Scene->OnAddForceAtPositionEvent(ForceEvent);

        World->DrawLine(AttachPoint, Target, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 2.5f, false);
        World->DrawSphere(Target, 0.035f, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 10, 2.0f, false);
    }

    IWorldEditorMode* FWorldEditorTool::GetActiveMode() const
    {
        if (EditorModes.empty()) return nullptr;
        const int32 Idx = Math::Clamp(ActiveModeIndex, 0, (int32)EditorModes.size() - 1);
        return EditorModes[Idx].get();
    }

    void FWorldEditorTool::SetActiveMode(int32 NewIndex)
    {
        if (EditorModes.empty()) return;
        NewIndex = Math::Clamp(NewIndex, 0, (int32)EditorModes.size() - 1);
        if (NewIndex == ActiveModeIndex) return;

        // bImGuizmoUsedOnce sticks true otherwise and blocks clicks after switching back.
        if (bImGuizmoUsedOnce)
        {
            EndTransaction("Transform");
            bImGuizmoUsedOnce = false;
        }
        bVertexSnapAnchorValid = false;
        bVertexSnapApplied     = false;
        SelectionBox.bActive   = false;

        if (IWorldEditorMode* Old = EditorModes[ActiveModeIndex].get())
        {
            Old->OnExit(World);
        }
        ActiveModeIndex = NewIndex;
        if (IWorldEditorMode* New = EditorModes[ActiveModeIndex].get())
        {
            New->OnEnter(World);
        }
    }

    void FWorldEditorTool::OnEntityCreated(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        // @TODO MarkTreeDirty here is too expensive; outliner is updated incrementally.
    }

    void FWorldEditorTool::OnEntityScriptChanged(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        OutlinerListView.MarkTreeDirty();
    }

    FName FWorldEditorTool::GetToolName() const
    {
        if (World == nullptr)
        {
            return Super::GetToolName();
        }

        // A world that was never saved has no package and still carries its placeholder name.
        const FName Name = World->GetPackage() != nullptr ? World->GetName() : FName("Untitled");

        if (CachedWindowNameSource != Name)
        {
            CachedWindowNameSource = Name;

            // ImGui identifies the window by what follows, so opening a level renames this tab in place.
            CachedWindowName = Format("{0} {1}###WorldEditor",
                GetTitlebarIcon(), Name.c_str()).c_str();
        }

        return CachedWindowName;
    }

    const char* FWorldEditorTool::GetTitlebarIcon() const
    {
        return LE_ICON_EARTH;
    }

    void FWorldEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FEditorTool::DrawToolMenu(UpdateContext);

        DrawActionMenu(LE_ICON_AXIS_ARROW " Transform", "Transform");
        DrawActionMenu(LE_ICON_CURSOR_DEFAULT_OUTLINE " Selection", "Selection");
        DrawActionMenu(LE_ICON_VIDEO_OUTLINE " View", "View");
    }

    void FWorldEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Selection",
            "Click an entity in the viewport or outliner to select. Ctrl-click adds, Shift-click range-selects. "
            "Marquee-drag in the viewport for area selection. Esc clears.");
        DrawHelpTextRow("Gizmo",
            "W/E/R = Translate/Rotate/Scale. Spacebar cycles. X toggles World/Local space. "
            "Hold Ctrl during a translate drag for vertex-snap to nearest unselected mesh vertex.");
        DrawHelpTextRow("Snap",
            "Snap settings (translate/rotate/scale step) live under the snap popup in the viewport toolbar.");
        DrawHelpTextRow("Camera",
            "Right-click + WASD to fly. Mouse wheel adjusts speed. F frames the selection. "
            "Ctrl+1..9 saves a bookmark to that slot; 1..9 recalls it.");
        DrawHelpTextRow("Game View (G)",
            "Hides grid, billboards, AABBs, and gizmos so the viewport shows only what a runtime camera would. "
            "Restores your prior toggles on exit.");
        DrawHelpTextRow("Simulate / Play",
            "Simulate runs physics + scripts in-place; Play (PIE) duplicates the world and switches to it. "
            "Stop returns to the original editor world.");
        DrawHelpTextRow("Undo / Redo",
            "Ctrl+Z / Ctrl+Y. Each action that mutates the registry (transform edit, component add/remove, "
            "entity creation) is captured as a transaction.");
        DrawHelpTextRow("Drop to Floor",
            "Casts a ray downward from each selected entity's pivot using their CPU AABBs. "
            "No physics scene is required.");
        DrawHelpTextRow("Prefabs",
            "Right-click a selection > Create Prefab to author one; drag the asset back into the viewport "
            "or outliner to instantiate.");
        DrawHelpTextRow("Scripts",
            "Attach a C# script class to an entity from the right-click menu. The class name is matched "
            "against the loaded EntityScript types; rebuild the game assembly to refresh the list.");
    }

    void FWorldEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        // 75% viewport / 25% inspector column.
        ImGuiID dockLeft = 0, dockRight = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.25f, &dockRight, &dockLeft);

        // SplitNode takes parent, direction, ratio, out-at-direction and out-opposite, easy to swap.
        ImGuiID dockRightBottom = 0, dockRightTop = 0;
        ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Down, 0.35f, &dockRightBottom, &dockRightTop);

        // A bottom strip with Details and World Settings side by side.
        ImGuiID dockRightBottomLeft = 0, dockRightBottomRight = 0;
        ImGui::DockBuilderSplitNode(dockRightBottom, ImGuiDir_Right, 0.5f, &dockRightBottomRight, &dockRightBottomLeft);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(),    dockLeft);
        ImGui::DockBuilderDockWindow(GetToolWindowName(SceneGraphName).c_str(),        dockRightTop);
        ImGui::DockBuilderDockWindow(GetToolWindowName("Details").c_str(),             dockRightBottomLeft);
        ImGui::DockBuilderDockWindow(GetToolWindowName(WorldSettingsName).c_str(),     dockRightBottomRight);
        ImGui::DockBuilderDockWindow(GetToolWindowName(SystemsName).c_str(),           dockRightBottomRight);
    }

    void FWorldEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        // Shared with the game preview tool, so focus feedback reads the same in both.
        DrawGameFocusIndicator(ViewportSize);

        if (bViewportHovered)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Space))
            {
                CycleGuizmoOp();
            }
        }

        // It needs only the world's net state, so it draws before the editor-only gizmo gate below.

        if (World->IsGameWorld() || bGameViewMode)
        {
            return;
        }
        
        SCameraComponent& CameraComponent = ECS::GetWorldRegistry(*World).Get<SCameraComponent>(EditorEntity);

        FMatrix4 ViewMatrix = CameraComponent.GetViewMatrix();
        FMatrix4 ProjectionMatrix = CameraComponent.GetProjectionMatrix();
        // Camera projection bakes Vulkan +Y-down NDC; ImGuizmo expects GL convention.
        ProjectionMatrix[1][1] *= -1.0f;

        const ImVec2 ViewportOrigin = ViewportScreenMin;

        ImGuizmo::SetDrawlist(ImGui::GetCurrentWindow()->DrawList);
        ImGuizmo::SetRect(ViewportOrigin.x, ViewportOrigin.y, ViewportSize.x, ViewportSize.y);

        {
            const ImRect ViewportRect(ViewportOrigin, ImVec2(ViewportOrigin.x + ViewportSize.x, ViewportOrigin.y + ViewportSize.y));
            if (ImGui::BeginDragDropTargetCustom(ViewportRect, ImGui::GetCurrentWindow()->ID))
            {
                // Passing null unconditionally made every target-only handler dead in the viewport.
                FVector3     TracedLocation(0.0f);
                ECS::FEntity HitEntity = ECS::NullEntity;
                TraceViewportPlacement(ImGui::GetMousePos(), TracedLocation, &HitEntity);

                AcceptContentBrowserPrefabPayload(HitEntity, /*bAttachToTarget*/ false);
                ImGui::EndDragDropTarget();
            }
        }

        if (IWorldEditorMode* ActiveMode = GetActiveMode())
        {
            ActiveMode->Tick(World, CameraComponent, bViewportHovered, ViewportOrigin, ViewportSize);
            ActiveMode->DrawOverlay(World, ViewportOrigin, ViewportSize, CameraComponent);
        }

        UpdateSimulationGrab(ViewportOrigin, ViewportSize, bViewportHovered);

        // Shift and click is also the marquee chord, so a poke at a ragdoll would select everything crossed.
        const bool bModeOwnsInput = (GetActiveMode() && GetActiveMode()->ConsumesViewportInput())
                                 || IsSimulationGrabActive();

        // TickEditorCamera runs in Update before this draw, so the flag is current-frame.
        const bool bCameraOwnsInput = ShouldSuppressViewportClickInput();

        auto SelectionView = ECS::GetWorldRegistry(*World).View<FSelectedInEditorComponent, STransformComponent>();

        const ECS::FEntity PivotEntityForGizmo = GetLastSelectedEntity();
        const bool bGizmoTargetValid = SelectionView.Num() && ECS::GetWorldRegistry(*World).IsValid(PivotEntityForGizmo);

        // ImGuizmo never sees the release, so end the transaction and reset or clicks stay blocked.
        if (!bGizmoTargetValid && bImGuizmoUsedOnce)
        {
            EndTransaction("Transform");
            bImGuizmoUsedOnce = false;
            bVertexSnapAnchorValid = false;
            bVertexSnapApplied = false;
        }

        if (bGizmoTargetValid && !bModeOwnsInput)
        {
            {
                ECS::FEntity PivotEntity = PivotEntityForGizmo;
                STransformComponent& PivotTransformComponent = ECS::GetWorldRegistry(*World).Get<STransformComponent>(PivotEntity);

                // Padded AABB so the gizmo stays visible when the pivot is just outside the frustum but handles aren't.
                const FVector3 PivotWorld = PivotTransformComponent.GetWorldLocation();
                const FAABB PivotBounds(PivotWorld - FVector3(1.0f), PivotWorld + FVector3(1.0f));
                const bool bPivotVisible = CameraComponent.GetViewVolume().GetFrustum().IsInside(PivotBounds);

                // Mid-drag stays drawn so ImGuizmo's release fires; otherwise bImGuizmoUsedOnce sticks and IsOver() blocks clicks.
                if (bPivotVisible || bImGuizmoUsedOnce)
                {
                    FMatrix4 EntityMatrix = PivotTransformComponent.GetWorldMatrix();

                    float* SnapValues = nullptr;
                    float SnapArray[3] = {};

                    if (bGuizmoSnapEnabled)
                    {
                        switch (GuizmoOp)
                        {
                        case ImGuizmo::TRANSLATE:
                            SnapArray[0] = GuizmoSnapTranslate;
                            SnapArray[1] = GuizmoSnapTranslate;
                            SnapArray[2] = GuizmoSnapTranslate;
                            SnapValues = SnapArray;
                            break;

                        case ImGuizmo::ROTATE:
                            SnapArray[0] = GuizmoSnapRotate;
                            SnapArray[1] = GuizmoSnapRotate;
                            SnapArray[2] = GuizmoSnapRotate;
                            SnapValues = SnapArray;
                            break;

                        case ImGuizmo::SCALE:
                            SnapArray[0] = GuizmoSnapScale;
                            SnapArray[1] = GuizmoSnapScale;
                            SnapArray[2] = GuizmoSnapScale;
                            SnapValues = SnapArray;
                            break;

                        // The editor only ever cycles between the three whole-transform modes.
                        default:
                            break;
                        }
                    }

                    FMatrix4 PreManipulateMatrix = EntityMatrix;
                    
                    const bool bCtrlHeld = ImGui::GetIO().KeyCtrl;
                    const bool bVertexSnapArmed = bCtrlHeld
                                               && !bCtrlConsumedByChord
                                               && GuizmoOp == ImGuizmo::TRANSLATE
                                               && GuizmoMode == ImGuizmo::WORLD
                                               && !World->IsGameWorld();

                    if (!bVertexSnapArmed)
                    {
                        bVertexSnapAnchorValid = false;
                    }
                    const FMatrix4 SnapViewProj = ProjectionMatrix * ViewMatrix;
                    FVector3 PreviewAnchorLocal(0.0f);
                    FVector3 PreviewAnchorWorld(0.0f);
                    bool bPreviewAnchorValid = false;
                    if (bVertexSnapArmed)
                    {
                        const SStaticMeshComponent* PivotMC = ECS::GetWorldRegistry(*World).TryGet<SStaticMeshComponent>(PivotEntity);
                        if (PivotMC && PivotMC->StaticMesh)
                        {
                            const ImVec2 MP = ImGui::GetMousePos();
                            const ImVec2 MouseInViewport(MP.x - ViewportOrigin.x, MP.y - ViewportOrigin.y);
                            bPreviewAnchorValid = FindClosestVertexToScreenPoint(
                                *PivotMC->StaticMesh.Get(), PreManipulateMatrix, SnapViewProj,
                                ViewportSize, MouseInViewport, FLT_MAX,
                                PreviewAnchorLocal, PreviewAnchorWorld);
                        }
                    }

                    // Never applied mid-drag, since clearing the using state without a release strands the transaction.
                    const bool bGizmoInert = bCameraOwnsInput && !bImGuizmoUsedOnce;
                    if (bGizmoInert)
                    {
                        ImGuizmo::Enable(false);
                    }

                    FMatrix4 GizmoDeltaMatrix(1.0f);
                    ImGuizmo::Manipulate(Math::ValuePtr(ViewMatrix), Math::ValuePtr(ProjectionMatrix),
                        GuizmoOp, GuizmoMode, Math::ValuePtr(EntityMatrix), Math::ValuePtr(GizmoDeltaMatrix), SnapValues);

                    if (bGizmoInert)
                    {
                        ImGuizmo::Enable(true);
                    }

                    if (ImGuizmo::IsUsing())
                    {
                        if (!bImGuizmoUsedOnce)
                        {
                            // BeginTransaction serializes the whole registry twice per drag.
                            TVector<ECS::FEntity> Dragged;
                            Dragged.reserve(SelectionView.Num());
                            for (ECS::FEntity Selected : SelectionView)
                            {
                                Dragged.push_back(Selected);
                            }
                            BeginTransformTransaction(Dragged);

                            bImGuizmoUsedOnce = true;
                            // Click landed on the gizmo, not empty space, kill the marquee armed by IsMouseClicked.
                            SelectionBox.bActive = false;
                        }
                        
                        const FMatrix4& DeltaMatrix = GizmoDeltaMatrix;

                        FVector3 DeltaTranslation, DeltaScale, DeltaSkew;
                        FQuat DeltaRotation;
                        FVector4 DeltaPerspective;
                        Math::Decompose(DeltaMatrix, DeltaScale, DeltaRotation, DeltaTranslation, DeltaSkew, DeltaPerspective);

                        // Override DeltaTranslation to align the anchor vertex to the closest non-selected vertex.
                        bVertexSnapApplied = false;
                        if (bVertexSnapArmed)
                        {
                            // Lock in the preview anchor on first armed frame of the drag.
                            if (!bVertexSnapAnchorValid && bPreviewAnchorValid)
                            {
                                VertexSnapAnchorLocal  = PreviewAnchorLocal;
                                bVertexSnapAnchorValid = true;
                            }

                            if (bVertexSnapAnchorValid)
                            {
                                ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
                                const FVector3 AnchorPreWorld = FVector3(PreManipulateMatrix * FVector4(VertexSnapAnchorLocal, 1.0f));
                                const FVector3 AnchorCandidateWorld = AnchorPreWorld + DeltaTranslation;

                                ImVec2 AnchorScreen;
                                if (ProjectPointToScreen(AnchorCandidateWorld, SnapViewProj, ViewportSize, AnchorScreen))
                                {
                                    float BestDistSq = VertexSnapPixelRadius * VertexSnapPixelRadius;
                                    FVector3 BestTargetWorld(0.0f);
                                    bool bFoundTarget = false;

                                    Registry.View<SStaticMeshComponent, STransformComponent>().ForEach(
                                        [&](ECS::FEntity Entity, SStaticMeshComponent& MeshComp, STransformComponent& Xform)
                                    {
                                        if (Entity == EditorEntity || !MeshComp.StaticMesh)
                                        {
                                            return;
                                        }
                                        if (Registry.HasAll<FSelectedInEditorComponent>(Entity))
                                        {
                                            return;
                                        }

                                        FVector3 LP, WP;
                                        if (!FindClosestVertexToScreenPoint(*MeshComp.StaticMesh.Get(),
                                                                            Xform.GetWorldMatrix(), SnapViewProj,
                                                                            ViewportSize, AnchorScreen,
                                                                            VertexSnapPixelRadius, LP, WP))
                                        {
                                            return;
                                        }
                                        ImVec2 HitScreen;
                                        if (!ProjectPointToScreen(WP, SnapViewProj, ViewportSize, HitScreen)) return;
                                        const float dx = HitScreen.x - AnchorScreen.x;
                                        const float dy = HitScreen.y - AnchorScreen.y;
                                        const float DistSq = dx * dx + dy * dy;
                                        if (DistSq < BestDistSq)
                                        {
                                            BestDistSq      = DistSq;
                                            BestTargetWorld = WP;
                                            bFoundTarget    = true;
                                        }
                                    });

                                    if (bFoundTarget)
                                    {
                                        DeltaTranslation       = BestTargetWorld - AnchorPreWorld;
                                        bVertexSnapApplied     = true;
                                        VertexSnapTargetWorld  = BestTargetWorld;
                                        VertexSnapAnchorWorld  = BestTargetWorld;
                                    }
                                    else
                                    {
                                        VertexSnapAnchorWorld = AnchorCandidateWorld;
                                    }
                                }
                            }
                        }
                        else
                        {
                            bVertexSnapAnchorValid = false;
                        }

                        if (GuizmoMode == ImGuizmo::LOCAL)
                        {
                            FMatrix4 LocalDeltaMatrix = Math::Inverse(PreManipulateMatrix) * EntityMatrix;

                            FVector3 LocalDeltaTrans, LocalDeltaScaleVec, LocalDeltaSkew;
                            FQuat LocalDeltaRot;
                            FVector4 LocalDeltaPersp;
                            const bool bLocalDeltaValid = Math::Decompose(
                                LocalDeltaMatrix, LocalDeltaScaleVec, LocalDeltaRot, LocalDeltaTrans, LocalDeltaSkew, LocalDeltaPersp);

                            SelectionView.ForEach([&](ECS::FEntity, STransformComponent& Transform)
                            {
                                if (!bLocalDeltaValid)
                                {
                                    return;
                                }

                                switch (GuizmoOp)
                                {
                                    case ImGuizmo::TRANSLATE:
                                    {
                                        // Delta is in entity-local axes; rotate into parent space before adding.
                                        FVector3 ParentSpaceDelta = Transform.GetLocalRotation() * LocalDeltaTrans;
                                        Transform.SetLocalLocation(Transform.GetLocalLocation() + ParentSpaceDelta);
                                        break;
                                    }

                                    case ImGuizmo::ROTATE:
                                    {
                                        Transform.SetLocalRotation(Math::Normalize(Transform.GetLocalRotation() * LocalDeltaRot));
                                        break;
                                    }

                                    case ImGuizmo::SCALE:
                                    {
                                        Transform.SetLocalScale(SanitizeManipulationScale(Transform.GetLocalScale() * LocalDeltaScaleVec));
                                        break;
                                    }

                                // The editor only ever cycles between the three whole-transform modes.
                                default:
                                    break;
                                }
                            });
                        }
                        else
                        {
                            FVector3 PivotPosition = PivotTransformComponent.GetWorldLocationCached();

                            SelectionView.ForEach([&](ECS::FEntity Entity, STransformComponent& Transform)
                            {
                                FMatrix4 DesiredWorldMatrix;

                                switch (GuizmoOp)
                                {
                                    case ImGuizmo::TRANSLATE:
                                    {
                                        FMatrix4 TranslationDelta = Math::Translate(FMatrix4(1.f), DeltaTranslation);
                                        DesiredWorldMatrix = TranslationDelta * Transform.GetWorldMatrix();
                                        break;
                                    }

                                    case ImGuizmo::ROTATE:
                                    {
                                        FVector3 OffsetFromPivot = Transform.GetWorldLocationCached() - PivotPosition;
                                        FVector3 RotatedOffset   = DeltaRotation * OffsetFromPivot;
                                        FVector3 NewWorldPos     = PivotPosition + RotatedOffset;
                                        FQuat NewWorldRot     = DeltaRotation * Transform.GetWorldRotation();
                                        FVector3 WorldScale      = Transform.GetWorldScale();

                                        DesiredWorldMatrix = Math::Translate(FMatrix4(1.f), NewWorldPos)
                                                           * Math::ToMatrix4(NewWorldRot)
                                                           * Math::Scale(FMatrix4(1.f), WorldScale);
                                        break;
                                    }

                                    case ImGuizmo::SCALE:
                                    {
                                        const FVector3 CurrentWorldScale = Transform.GetWorldScale();
                                        FVector3 ClampedDeltaScale       = DeltaScale;
                                        constexpr float MinScale          = 0.001f;
                                        for (int Axis = 0; Axis < 3; ++Axis)
                                        {
                                            const float Target = CurrentWorldScale[Axis] * DeltaScale[Axis];
                                            if (!std::isfinite(Target) || Math::Abs(Target) < MinScale)
                                            {
                                                const float SignedMin = (Target < 0.0f) ? -MinScale : MinScale;
                                                ClampedDeltaScale[Axis] = (Math::Abs(CurrentWorldScale[Axis]) > 1e-8f)
                                                                        ? SignedMin / CurrentWorldScale[Axis]
                                                                        : 1.0f;
                                            }
                                        }

                                        FVector3 OffsetFromPivot = Transform.GetWorldLocationCached() - PivotPosition;
                                        FVector3 ScaledOffset    = OffsetFromPivot * ClampedDeltaScale;
                                        FVector3 NewWorldPos     = PivotPosition + ScaledOffset;
                                        FQuat WorldRot        = Transform.GetWorldRotation();
                                        FVector3 NewWorldScale   = CurrentWorldScale * ClampedDeltaScale;

                                        DesiredWorldMatrix = Math::Translate(FMatrix4(1.f), NewWorldPos)
                                                           * Math::ToMatrix4(WorldRot)
                                                           * Math::Scale(FMatrix4(1.f), NewWorldScale);
                                        break;
                                    }

                                // The editor only ever cycles between the three whole-transform modes.
                                default:
                                    break;
                                }

                                FRelationshipComponent* Rel = ECS::GetWorldRegistry(*World).TryGet<FRelationshipComponent>(Entity);
                                if (Rel && Rel->Parent != ECS::NullEntity)
                                {
                                    STransformComponent& ParentTransform = ECS::GetWorldRegistry(*World).Get<STransformComponent>(Rel->Parent);
                                    FMatrix4 LocalMatrix = Math::Inverse(ParentTransform.GetWorldMatrix()) * DesiredWorldMatrix;

                                    FVector3 LocalTranslation, LocalScale, LocalSkew;
                                    FQuat LocalRotation;
                                    FVector4 LocalPerspective;

                                    if (!Math::Decompose(LocalMatrix, LocalScale, LocalRotation, LocalTranslation, LocalSkew, LocalPerspective))
                                    {
                                        return;
                                    }

                                    Transform.SetLocalLocation(LocalTranslation);
                                    Transform.SetLocalRotation(LocalRotation);
                                    Transform.SetLocalScale(SanitizeManipulationScale(LocalScale));
                                }
                                else
                                {
                                    FVector3 WorldTranslation, WorldScale, WorldSkew;
                                    FQuat WorldRotation;
                                    FVector4 WorldPerspective;
                                    if (!Math::Decompose(DesiredWorldMatrix, WorldScale, WorldRotation, WorldTranslation, WorldSkew, WorldPerspective))
                                    {
                                        return;
                                    }

                                    Transform.SetLocalLocation(WorldTranslation);
                                    Transform.SetLocalRotation(WorldRotation);
                                    Transform.SetLocalScale(SanitizeManipulationScale(WorldScale));
                                }
                            });
                        }

                        // The cached dirty signal can fail in a duplicated world.
                        for (ECS::FEntity Selected : SelectionView)
                        {
                            ECS::GetWorldRegistry(*World).EmplaceOrReplace<FNeedsTransformUpdate>(Selected);
                        }
                    }
                    else if (bImGuizmoUsedOnce)
                    {
                        EndTransaction("Transform");
                        bImGuizmoUsedOnce = false;
                        bVertexSnapAnchorValid = false;
                        bVertexSnapApplied     = false;
                    }

                    // A hint banner and anchor marker, with the locked target drawn while snapping.
                    if (bVertexSnapArmed)
                    {
                        ImDrawList* DL = ImGui::GetCurrentWindow()->DrawList;

                        const ImU32 ArmedCol = IM_COL32(120, 200, 255, 255);
                        const ImU32 SnapCol  = IM_COL32(255, 220,   0, 255);

                        const ImVec2 BannerPos(ViewportOrigin.x + 8.0f, ViewportOrigin.y + 8.0f);
                        const char* Label = bVertexSnapApplied ? "VERTEX SNAP" : "VERTEX SNAP (armed)";
                        const ImVec2 TextSize = ImGui::CalcTextSize(Label);
                        DL->AddRectFilled(BannerPos,
                            ImVec2(BannerPos.x + TextSize.x + 12.0f, BannerPos.y + TextSize.y + 6.0f),
                            IM_COL32(0, 0, 0, 160), 3.0f);
                        DL->AddText(ImVec2(BannerPos.x + 6.0f, BannerPos.y + 3.0f),
                            bVertexSnapApplied ? SnapCol : ArmedCol, Label);

                        // Live anchor marker.
                        FVector3 AnchorWorld(0.0f);
                        bool bHaveAnchor = false;
                        if (bVertexSnapAnchorValid)
                        {
                            AnchorWorld = bVertexSnapApplied
                                ? VertexSnapAnchorWorld
                                : FVector3(PivotTransformComponent.GetWorldMatrix() * FVector4(VertexSnapAnchorLocal, 1.0f));
                            bHaveAnchor = true;
                        }
                        else if (bPreviewAnchorValid)
                        {
                            AnchorWorld = PreviewAnchorWorld;
                            bHaveAnchor = true;
                        }

                        if (bHaveAnchor)
                        {
                            ImVec2 S;
                            if (ProjectPointToScreen(AnchorWorld, SnapViewProj, ViewportSize, S))
                            {
                                const ImVec2 P(S.x + ViewportOrigin.x, S.y + ViewportOrigin.y);
                                const ImU32 C = bVertexSnapApplied ? SnapCol : ArmedCol;
                                DL->AddRectFilled(ImVec2(P.x - 3, P.y - 3), ImVec2(P.x + 3, P.y + 3), C);
                                DL->AddRect(ImVec2(P.x - 7, P.y - 7), ImVec2(P.x + 7, P.y + 7), C, 0.0f, 0, 2.0f);
                            }
                        }

                        // Snap-target marker.
                        if (bVertexSnapApplied)
                        {
                            ImVec2 T;
                            if (ProjectPointToScreen(VertexSnapTargetWorld, SnapViewProj, ViewportSize, T))
                            {
                                const ImVec2 P(T.x + ViewportOrigin.x, T.y + ViewportOrigin.y);
                                DL->AddCircleFilled(P, 5.0f, SnapCol);
                                DL->AddCircle(P, 10.0f, SnapCol, 0, 2.0f);
                            }
                        }
                    }
                }
            }
        }

        // After the gizmo, which owns the mouse first whenever it is hovered or being dragged.
        DrawComponentVisualizerOverlay(ViewportOrigin, ViewportSize, CameraComponent,
            !bModeOwnsInput && !bCameraOwnsInput && !bCameraPreviewMouseOver && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing());

        // Drops any armed marquee, so releasing cannot commit a box selection nobody asked for.
        if (bCameraOwnsInput || AreVisualizersCapturingInput())
        {
            SelectionBox.bActive = false;
        }

        // The camera-preview grip flag is set by the overlay below, so it lags one frame.
        if (!bModeOwnsInput && !bCameraOwnsInput && !bCameraPreviewMouseOver && !AreVisualizersCapturingInput() && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && !RmlUi::WorldUIWantsMouse(World))
        {
            uint32 PickerWidth = World->GetRenderer()->GetRenderExtent().x;
            uint32 PickerHeight = World->GetRenderer()->GetRenderExtent().y;
            
            ImVec2 viewportScreenPos = ImGui::GetWindowPos();
            ImVec2 mousePos = ImGui::GetMousePos();

            ImVec2 MousePosInViewport;
            MousePosInViewport.x = mousePos.x - viewportScreenPos.x;
            MousePosInViewport.y = mousePos.y - viewportScreenPos.y;

            MousePosInViewport.x = Math::Clamp(MousePosInViewport.x, 0.0f, ViewportSize.x - 1.0f);
            MousePosInViewport.y = Math::Clamp(MousePosInViewport.y, 0.0f, ViewportSize.y - 1.0f);

            float ScaleX = static_cast<float>(PickerWidth) / ViewportSize.x;
            float ScaleY = static_cast<float>(PickerHeight) / ViewportSize.y;

            uint32 TexX = static_cast<uint32>(MousePosInViewport.x * ScaleX);
            uint32 TexY = static_cast<uint32>(MousePosInViewport.y * ScaleY);

            // Publish the cursor so the renderer copies just the window around it.
            World->GetRenderer()->SetPickerCursor(TexX, TexY, true);

            bool bOverImGuizmo = bImGuizmoUsedOnce ? ImGuizmo::IsOver() : false;

            // Intercepted here so the click assigns the reference instead of selecting.
            if (PickCtx.Broker->IsRequested())
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip(LE_ICON_EYEDROPPER " Click an entity to assign (Esc to cancel)");

                if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                {
                    PickCtx.Broker->Cancel();
                }
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    ECS::FEntity Hit = World->GetRenderer()->GetEntityAtPixel(TexX, TexY);
                    Hit = ResolveSelectionRootForViewportPick(ECS::GetWorldRegistry(*World), Hit);
                    if (Hit != ECS::NullEntity)
                    {
                        PickCtx.Broker->Fulfill(static_cast<uint32>((Hit).Value));
                    }
                }
            }
            else
            {
                ImVec2 RightDragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
                float RightDragDistance = sqrtf(RightDragDelta.x * RightDragDelta.x + RightDragDelta.y * RightDragDelta.y);
                // A right release short enough to be a tap rather than a camera look opens the context menu.
                bool bRightWasShortClick = RightDragDistance < 15.0f;

                // A right click is never a gizmo interaction, and the gizmo sits over the entity being asked about.
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                {
                    if (bRightWasShortClick)
                    {
                        ECS::FEntity EntityHandle = World->GetRenderer()->GetEntityAtPixel(TexX, TexY);
                        EntityHandle = ResolveSelectionRootForViewportPick(ECS::GetWorldRegistry(*World), EntityHandle);

                        // On a picker miss keep existing selection so the menu still has something to act on.
                        if (EntityHandle != ECS::NullEntity && !IsEntitySelected(EntityHandle))
                        {
                            SetSingleSelectedEntity(EntityHandle);
                            RevealEntityInOutliner(EntityHandle);
                        }

                        const ECS::FEntity MenuTarget = (EntityHandle != ECS::NullEntity)
                            ? EntityHandle
                            : GetLastSelectedEntity();

                        if (ECS::GetWorldRegistry(*World).IsValid(MenuTarget))
                        {
                            ImGui::OpenPopup("EntityContextMenu");
                        }
                    }
                }
            }

            // Unlike the context menu this DOES yield to the gizmo, which owns the pointer over a handle.
            if (!PickCtx.Broker->IsRequested() && !bOverImGuizmo)
            {
                ImVec2 LeftDragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                float LeftDragDistance = sqrtf(LeftDragDelta.x * LeftDragDelta.x + LeftDragDelta.y * LeftDragDelta.y);
                bool bLeftDragging = LeftDragDistance >= 15.0f;

                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    SelectionBox.bActive = true;
                    SelectionBox.Start = MousePosInViewport;
                    SelectionBox.Current = SelectionBox.Start;
                }

                if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && SelectionBox.bActive)
                {
                    SelectionBox.Current = MousePosInViewport;
                }

                if (SelectionBox.bActive && bLeftDragging)
                {
                    ImDrawList* DrawList = ImGui::GetWindowDrawList();
                    const ImVec2 Origin = ViewportOrigin;
                    const ImVec2 ScreenStart = ImVec2(Origin.x + SelectionBox.Start.x,   Origin.y + SelectionBox.Start.y);
                    const ImVec2 ScreenEnd   = ImVec2(Origin.x + SelectionBox.Current.x, Origin.y + SelectionBox.Current.y);
                    DrawList->AddRectFilled(ScreenStart, ScreenEnd, IM_COL32(100, 150, 255, 50));
                    DrawList->AddRect(ScreenStart, ScreenEnd, IM_COL32(100, 150, 255, 255), 0.0f, 0, 2.0f);
                }
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && SelectionBox.bActive)
                {
                    ImVec2 Start = SelectionBox.Start;
                    ImVec2 End = SelectionBox.Current;
                    
                    if (!bLeftDragging)
                    {
                        ECS::FEntity EntityHandle = World->GetRenderer()->GetEntityAtPixel(TexX, TexY);
                        EntityHandle = ResolveSelectionRootForViewportPick(ECS::GetWorldRegistry(*World), EntityHandle);

                        // Ctrl+click toggles picked entity in selection; plain click replaces.
                        if (ImGui::GetIO().KeyCtrl)
                        {
                            if (EntityHandle != ECS::NullEntity)
                            {
                                ToggleSelectedEntity(EntityHandle);
                            }
                        }
                        else
                        {
                            SetSingleSelectedEntity(EntityHandle);
                        }

                        // So clicking something in the world does not mean scrolling the tree to find it.
                        if (EntityHandle != ECS::NullEntity && IsEntitySelected(EntityHandle))
                        {
                            RevealEntityInOutliner(EntityHandle);
                        }
                    }
                    else
                    {
                        const ImVec2 RectMin(Math::Min(Start.x, End.x), Math::Min(Start.y, End.y));
                        const ImVec2 RectMax(Math::Max(Start.x, End.x), Math::Max(Start.y, End.y));
                        const FMatrix4 ViewProj = ProjectionMatrix * ViewMatrix;

                        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

                        THashSet<ECS::FEntity> Hits;
                        Registry.View<STransformComponent>().ForEach([&](ECS::FEntity Entity, STransformComponent& Transform)
                        {
                            if (Entity == EditorEntity)
                            {
                                return;
                            }

                            ImVec2 ProjMin, ProjMax;

                            if (const SStaticMeshComponent* Mesh = Registry.TryGet<SStaticMeshComponent>(Entity))
                            {
                                FAABB WorldAABB = Mesh->GetAABB().ToWorld(Transform.GetWorldMatrix());
                                if (!ProjectAABBToScreenRect(WorldAABB, ViewProj, ViewportSize, ProjMin, ProjMax))
                                {
                                    return;
                                }
                            }
                            else
                            {
                                const FVector3 P = Transform.GetWorldLocation();
                                FVector4 Clip = ViewProj * FVector4(P, 1.0f);
                                if (Clip.w <= 1e-4f)
                                {
                                    return;
                                }
                                const float Px = (Clip.x / Clip.w * 0.5f + 0.5f) * ViewportSize.x;
                                const float Py = (1.0f - (Clip.y / Clip.w * 0.5f + 0.5f)) * ViewportSize.y;
                                ProjMin = ImVec2(Px - 4.0f, Py - 4.0f);
                                ProjMax = ImVec2(Px + 4.0f, Py + 4.0f);
                            }

                            const bool bOverlap = ProjMax.x >= RectMin.x && ProjMin.x <= RectMax.x
                                               && ProjMax.y >= RectMin.y && ProjMin.y <= RectMax.y;
                            if (bOverlap)
                            {
                                Hits.insert(ResolveSelectionRootForViewportPick(Registry, Entity));
                            }
                        });

                        const bool bShift = ImGui::GetIO().KeyShift;
                        const bool bCtrl  = ImGui::GetIO().KeyCtrl;
                        if (!bShift && !bCtrl)
                        {
                            ClearSelectedEntities();
                        }

                        for (ECS::FEntity Hit : Hits)
                        {
                            if (bCtrl)
                            {
                                ToggleSelectedEntity(Hit);
                            }
                            else
                            {
                                AddSelectedEntity(Hit, false);
                            }
                        }
                    } 
    
                    SelectionBox.bActive = false;
                }
            }
        }
        else
        {
            // Not a pick target this frame, so tell the renderer to skip the picker readback.
            World->GetRenderer()->SetPickerCursor(0, 0, false);
        }

        if (ImGui::BeginPopup("EntityContextMenu"))
        {
            const ECS::FEntity LastSelected = GetLastSelectedEntity();

            if (ECS::GetWorldRegistry(*World).IsValid(LastSelected))
            {
                ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
                const bool bLastSelectedLocked = IsLockedPrefabChild(Registry, LastSelected);
                const size_t NumSelected = SelectedEntities.size();
                const bool bMultiSelected = NumSelected > 1;

                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));

                // A header of name and ID for the focal entity, with a more badge for a wider selection.
                {
                    const SNameComponent* HeaderName = Registry.TryGet<SNameComponent>(LastSelected);
                    FStringView HeaderText = HeaderName ? FStringView(HeaderName->Name.c_str()) : FStringView("<unnamed>");

                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                    ImGui::TextUnformatted(HeaderText.data(), HeaderText.data() + HeaderText.size());
                    ImGui::PopStyleColor();

                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("#%u", (uint32)LastSelected);
                    ImGui::PopStyleColor();

                    if (bMultiSelected)
                    {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
                        ImGui::Text("+%zu more", NumSelected - 1);
                        ImGui::PopStyleColor();
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Edit actions, the clipboard and duplicate.
                if (!bLastSelectedLocked)
                {
                    if (ImGui::MenuItem(LE_ICON_CONTENT_DUPLICATE " Duplicate", "Ctrl+D"))
                    {
                        BeginTransaction();
                        ECS::FEntity To = ECS::NullEntity;
                        CopyEntity(To, LastSelected);
                        if (To != ECS::NullEntity)
                        {
                            EndTransaction("Duplicate");
                        }
                        else
                        {
                            AbortTransaction();
                        }
                        ImGui::CloseCurrentPopup();
                    }
                }

                if (ImGui::MenuItem(LE_ICON_CONTENT_COPY " Copy", "Ctrl+C"))
                {
                    ClearCopies();
                    AddEntityToCopies(LastSelected);
                    ImGui::CloseCurrentPopup();
                }

                if (ImGui::MenuItem("Copy Entity ID"))
                {
                    ImGui::SetClipboardText(Format("{}", (LastSelected).Value).c_str());
                    ImGui::CloseCurrentPopup();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Components.
                if (ImGui::MenuItem("Add Component..."))
                {
                    PushAddComponentModal(LastSelected);
                    ImGui::CloseCurrentPopup();
                }

                DrawScriptAttachMenuItems(LastSelected);

                if (ImGui::BeginMenu("Remove Component"))
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));

                    ECS::Utils::ForEachComponent(Registry, LastSelected, [&](void*, const ECS::FSparseSet&, CStruct* StructType)
                    {
                        if (StructType == SNameComponent::StaticStruct() || StructType == STransformComponent::StaticStruct())
                        {
                            return;
                        }

                        if (ImGui::MenuItem(StructType->MakeDisplayName().c_str()))
                        {
                            ComponentDestroyRequests.push(FComponentDestroyRequest{StructType, LastSelected});
                        }
                    });

                    ImGui::PopStyleColor();
                    ImGui::PopStyleVar();
                    ImGui::EndMenu();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Hierarchy.
                if (!bLastSelectedLocked && bMultiSelected)
                {
                    if (ImGui::MenuItem(LE_ICON_FOLDER " Group Selection", "Ctrl+G"))
                    {
                        GroupSelectedEntities();
                        ImGui::CloseCurrentPopup();
                    }
                }

                if (!bLastSelectedLocked && ECS::Utils::IsChild(Registry, LastSelected))
                {
                    if (ImGui::MenuItem("Unparent"))
                    {
                        BeginRelationshipTransaction({ LastSelected });
                        ECS::Utils::RemoveFromParent(Registry, LastSelected);
                        EndTransaction("Unparent");
                        ReparentEntityInOutliner(LastSelected);
                        ImGui::CloseCurrentPopup();
                    }
                }

                if (!bLastSelectedLocked && ECS::Utils::IsParent(Registry, LastSelected))
                {
                    if (ImGui::MenuItem("Detach Children"))
                    {
                        TVector<ECS::FEntity> Children;
                        ECS::Utils::ForEachChild(Registry, LastSelected, [&](ECS::FEntity Child) { Children.push_back(Child); });
                        BeginRelationshipTransaction({ LastSelected });
                        ECS::Utils::DetachImmediateChildren(Registry, LastSelected);
                        EndTransaction("Detach Children");
                        for (ECS::FEntity Child : Children)
                        {
                            ReparentEntityInOutliner(Child);
                        }
                        ImGui::CloseCurrentPopup();
                    }
                }

                if (!bLastSelectedLocked)
                {
                    const bool bIsSelectionRoot = Registry.HasAll<FSelectionRoot>(LastSelected);
                    if (ImGui::MenuItem(bIsSelectionRoot ? "Unmark Selection Root" : "Mark as Selection Root"))
                    {
                        if (bIsSelectionRoot)
                        {
                            Registry.Remove<FSelectionRoot>(LastSelected);
                        }
                        else
                        {
                            Registry.Emplace<FSelectionRoot>(LastSelected);
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGuiX::TextTooltip("{}", "Viewport clicks on any descendant will resolve up to this entity.");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Prefab.
                if (ImGui::MenuItem(LE_ICON_PACKAGE_VARIANT " Create Prefab from Selected..."))
                {
                    PushCreatePrefabFromSelectionModal();
                    ImGui::CloseCurrentPopup();
                }
                ImGuiX::TextTooltip("{}", "Save the selection as a reusable prefab asset. Children of selected entities are included automatically.");

                if (const SPrefabInstanceComponent* Instance = Registry.TryGet<SPrefabInstanceComponent>(LastSelected);
                    Instance != nullptr && Instance->bIsRoot)
                {
                    if (ImGui::MenuItem(LE_ICON_SYNC " Resync from Prefab", nullptr, false, Instance->SourcePrefab != nullptr))
                    {
                        ResyncPrefabInstance(LastSelected);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGuiX::TextTooltip("{}", "Re-apply the source prefab to this instance now. Per-instance overrides and the placed transform are kept.");

                    if (ImGui::MenuItem(LE_ICON_LINK_VARIANT_OFF " Detach from Prefab"))
                    {
                        DetachPrefabInstance(LastSelected);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGuiX::TextTooltip("{}", "Unlink this instance from its source prefab; the entities become plain and stop syncing.");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Destructive at bottom.
                if (!bLastSelectedLocked)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    if (ImGui::MenuItem(LE_ICON_TRASH_CAN " Delete Entity", "Del"))
                    {
                        if (Dialogs::Confirmation("Confirm Deletion", "Are you sure you want to delete entity \"{0}\"?\n\nThis action cannot be undone.", (LastSelected).Value))
                        {
                            EntityDestroyRequests.push(LastSelected);
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor();
                }

                ImGui::Spacing();

                ImGui::PopStyleVar(3);
            }

            ImGui::EndPopup();
        }

        DrawCursorWorldPositionOverlay(ViewportOrigin, ViewportSize, CameraComponent);
        DrawEntityDebugOverlay(ViewportOrigin, ViewportSize, CameraComponent);
        DrawOffscreenSelectionIndicators(ViewportOrigin, ViewportSize, CameraComponent);

        DrawCameraPreviewOverlay(ViewportOrigin, ViewportSize);
    }

    // The viewport overlay toolbar lives in FSceneEditorTool; the world editor supplies these hooks.
    bool FWorldEditorTool::IsViewportPlaying() const
    {
        return bGamePreviewRunning;
    }

    void FWorldEditorTool::PersistGizmoSettings()
    {
        CWorldEditorSettings* Settings = GetMutableDefault<CWorldEditorSettings>();
        Settings->bGizmoSnapEnabled  = bGuizmoSnapEnabled;
        Settings->GizmoSnapTranslate = GuizmoSnapTranslate;
        Settings->GizmoSnapRotate    = GuizmoSnapRotate;
        Settings->GizmoSnapScale     = GuizmoSnapScale;
        GConfig->SaveSettings(CWorldEditorSettings::StaticClass());
    }

    void FWorldEditorTool::PersistCameraPreviewScale()
    {
        CWorldEditorSettings* Settings = GetMutableDefault<CWorldEditorSettings>();
        Settings->CameraPreviewScale = CameraPreviewScale;
        GConfig->SaveSettings(CWorldEditorSettings::StaticClass());
    }

    void FWorldEditorTool::DrawViewportToolbarPlayControls(float ButtonSize)
    {
        DrawSimulationControls(ButtonSize);

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
    }

    void FWorldEditorTool::DrawViewportToolbarModeSelector(float ButtonSize)
    {
        // The active mode appends its own toolbar, so the bar stays clean regardless of mode count.
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        if (IWorldEditorMode* Active = GetActiveMode())
        {
            char Preview[64];
            ImFormatString(Preview, sizeof(Preview), "%s  %s", Active->GetIcon(), Active->GetDisplayName());

            const float PadY = Math::Max(ImGui::GetStyle().FramePadding.y, (ButtonSize - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, PadY));

            const float ComboWidth = ImGui::CalcTextSize(Preview).x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
            ImGui::SetNextItemWidth(ComboWidth);
            const bool bComboOpen = ImGui::BeginCombo("##EditorMode", Preview);
            ImGui::PopStyleVar();
            if (bComboOpen)
            {
                for (int32 Idx = 0; Idx < (int32)EditorModes.size(); ++Idx)
                {
                    IWorldEditorMode* Mode = EditorModes[Idx].get();
                    if (!Mode)
                    {
                        continue;
                    }
                    const bool bSelected = (Idx == ActiveModeIndex);

                    char Label[64];
                    ImFormatString(Label, sizeof(Label), "%s  %s", Mode->GetIcon(), Mode->GetDisplayName());
                    if (ImGui::Selectable(Label, bSelected))
                    {
                        SetActiveMode(Idx);
                    }
                    if (const char* Tip = Mode->GetTooltip())
                    {
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("%s", Tip);
                        }
                    }
                    if (bSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }

        if (IWorldEditorMode* ActiveMode = GetActiveMode())
        {
            ActiveMode->DrawToolbar(World, ButtonSize);
        }
    }

    void FWorldEditorTool::DrawViewModeExtraItems()
    {
        ImGui::MenuItem("Draw Entity Debug Info", nullptr, &bDrawEntityDebugInfo);
        ImGui::MenuItem("Network (AOI / Grid)", nullptr, &bDrawNetworkDebug);

        // The bool overload would flip the flag without saving and restoring grid and billboard state.
        if (ImGui::MenuItem("Game View", "G", bGameViewMode))
        {
            ToggleGameViewMode();
        }
    }

    void FWorldEditorTool::PushAddTagModal(ECS::FEntity Entity)
    {
        struct FTagModalState
        {
            char TagBuffer[256] = {0};
            bool bTagExists = false;
        };
        
        TUniquePtr<FTagModalState> State = MakeUnique<FTagModalState>();
        
        ToolContext->PushModal("Add Tag", ImVec2(400.0f, 180.0f), [this, Entity, State = Move(State)] () -> bool
        {
            bool bTagAdded = false;
    
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
            ImGui::TextUnformatted("Enter a tag name for this entity");
            ImGui::PopStyleColor();
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
    
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.16f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.18f, 0.18f, 0.19f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.2f, 0.2f, 0.21f, 1.0f));
            
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            
            bool bInputEnter = ImGui::InputTextWithHint(
                "##TagInput",
                LE_ICON_TAG " Tag name...",
                State->TagBuffer,
                sizeof(State->TagBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue
            );
            
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere(-1);
            }
            
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
            
            FString TagName(State->TagBuffer);
            State->bTagExists = !TagName.empty() && ECS::Utils::EntityHasTag(TagName, ECS::GetWorldRegistry(*World), Entity);
            
            if (State->bTagExists)
            {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.4f, 0.4f, 1.0f));
                ImGui::TextUnformatted(LE_ICON_ALERT_CIRCLE " Tag already exists on this entity");
                ImGui::PopStyleColor();
            }
    
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
    
            constexpr float buttonWidth = 100.0f;
            float const buttonSpacing = ImGui::GetStyle().ItemSpacing.x;
            float const totalWidth = buttonWidth * 2 + buttonSpacing;
            float const availWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX((availWidth - totalWidth) * 0.5f);
            
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(20, 8));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            
            bool const bCanAdd = !TagName.empty() && !State->bTagExists;
            
            if (!bCanAdd)
            {
                ImGui::BeginDisabled();
            }

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.65f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.5f, 0.25f, 1.0f));
            
            if (ImGui::Button("Add", ImVec2(buttonWidth, 0)) || (bInputEnter && bCanAdd))
            {
                const FName TagId(TagName);
                auto Storage = ECS::GetWorldRegistry(*World).NamedStorage<STagComponent>(TagId);
                Storage.Emplace(Entity).Tag = TagId;
                bTagAdded = true;
            }
            
            ImGui::PopStyleColor(3);
            
            if (!bCanAdd)
            {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();
            
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.27f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.32f, 1.0f));
            
            bool bShouldClose = false;
            if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)))
            {
                bShouldClose = true;
            }
            
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
            
            return bTagAdded || bShouldClose;
        });
    }

    void FWorldEditorTool::PushAddComponentModal(ECS::FEntity Entity)
    {
        TUniquePtr<ImGuiTextFilter> Filter = MakeUnique<ImGuiTextFilter>();
        ToolContext->PushModal("Add Component", ImVec2(500.0f, 600.0f), [this, Entity, Filter = Move(Filter)] () -> bool
        {
            bool bComponentAdded = false;

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
            if (SelectedEntities.size() > 1 && IsEntitySelected(Entity))
            {
                ImGui::Text("Select a component to add to %llu selected entities", (unsigned long long)SelectedEntities.size());
            }
            else
            {
                ImGui::TextUnformatted("Select a component to add to the entity");
            }
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.16f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.18f, 0.18f, 0.19f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.2f, 0.2f, 0.21f, 1.0f));

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            Filter->Draw(LE_ICON_BRIEFCASE_SEARCH " Search Components...", ImGui::GetContentRegionAvail().x);

            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);

            ImGui::Spacing();

            const float ListHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2;

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 16.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));

            if (ImGui::BeginChild("##ComponentsList", ImVec2(0, ListHeight), true))
            {
                CStruct* PickedMetaType = nullptr;
                CStruct* PickedStruct    = nullptr;

                // Resolved before the list is drawn so it can gray out what these targets already have.
                TVector<ECS::FEntity> Targets = GetComponentEditTargets(Entity);

                if (DrawAddableComponentList(*Filter, Targets, PickedMetaType, PickedStruct))
                {
                    ApplyAddComponentToTargets(Targets, PickedMetaType);

                    bComponentAdded = true;
                }
            }
            ImGui::EndChild();

            ImGui::PopStyleVar(2);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const float ButtonWidth = 120.0f;
            const float AvailWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX((AvailWidth - ButtonWidth) * 0.5f);

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(20, 8));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.27f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.32f, 1.0f));

            bool bShouldClose = false;
            if (ImGui::Button("Cancel", ImVec2(ButtonWidth, 0)))
            {
                bShouldClose = true;
            }

            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);

            if (bComponentAdded && Entity == DetailsEntity)
            {
                bDetailsDirty = true;
            }

            return bComponentAdded || bShouldClose;
        });
    }

    void FWorldEditorTool::DrawScriptAttachMenuItems(ECS::FEntity Entity)
    {
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        if (!Registry.IsValid(Entity))
        {
            return;
        }

        const bool bHasScript = Registry.HasAll<SEntityScriptComponent>(Entity);

        // Always offered, since it either assigns a script or swaps the one already on the entity.
        TVector<FString> Types;
        DotNet::GatherEntityScriptTypes(Types);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(bHasScript ? LE_ICON_LANGUAGE_CSHARP " Change Script" : LE_ICON_LANGUAGE_CSHARP " Attach Script");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260.0f);
        const int32 Picked = ImGuiX::SearchableCombo("##AssignScript", bHasScript ? "Swap to..." : "Select a script...",
            (int32)Types.size(), INDEX_NONE,
            [&Types](int32 Index) { return FFixedString(Types[Index].c_str(), Types[Index].size()); }, LE_ICON_LANGUAGE_CSHARP);

        if (Picked != INDEX_NONE)
        {
            AttachScriptToEntity(Entity, Types[Picked]);
            ImGui::CloseCurrentPopup();
        }

        if (bHasScript)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
            if (ImGui::MenuItem(LE_ICON_TRASH_CAN " Remove Script"))
            {
                RemoveScriptFromEntity(Entity);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGuiX::TextTooltip("{}", "Remove the C# script component from this entity.");
        }
    }

    void FWorldEditorTool::AttachScriptToEntity(ECS::FEntity Entity, const FString& ScriptClass)
    {
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (!Registry.IsValid(Entity) || ScriptClass.empty())
        {
            return;
        }

        const bool bHad = Registry.HasAll<SEntityScriptComponent>(Entity);

        BeginComponentTransaction({ Entity }, SEntityScriptComponent::StaticStruct());
        // Attaches by class, the same call for a C++ script and a C# one, with OnReady on the next tick.
        EntityScripts::Attach(Registry, Entity, FindObject<CClass>(FName(ScriptClass)));
        EndTransaction(bHad ? "Add Script" : "Attach Script");

        if (World->GetPackage() != nullptr)
        {
            World->GetPackage()->MarkDirty();
        }
        bDetailsDirty = true;
        // on_construct<SEntityScriptComponent> refreshes the outliner on a first attach; harmless to mark again.
        OutlinerListView.MarkTreeDirty();
    }

    void FWorldEditorTool::RemoveScriptFromEntity(ECS::FEntity Entity)
    {
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (!Registry.IsValid(Entity) || !Registry.HasAll<SEntityScriptComponent>(Entity))
        {
            return;
        }

        BeginComponentTransaction({ Entity }, SEntityScriptComponent::StaticStruct());
        Registry.Remove<SEntityScriptComponent>(Entity); // on_destroy refreshes the outliner
        EndTransaction("Remove Script");

        if (World->GetPackage() != nullptr)
        {
            World->GetPackage()->MarkDirty();
        }
        bDetailsDirty = true;
    }

    CPackage* FWorldEditorTool::GetScenePackage() const
    {
        return World ? World->GetPackage() : nullptr;
    }

    void FWorldEditorTool::OnSave()
    {
        // A transient world has no package yet; prompt for a destination instead of saving.
        if (!World->GetPackage())
        {
            PushSaveAsAssetModal();
            return;
        }

        // The world editor saves its live CWorld in place, and the thumbnail comes from the viewport.
        if (ShouldGenerateThumbnailOnSave())
        {
            GenerateThumbnail(World->GetPackage());
        }

        FCoreEditorDelegates::OnAssetPreSave.Broadcast(World);

        if (CPackage::SavePackage(World->GetPackage(), World->GetPackage()->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetSaved(World);
            FCoreEditorDelegates::OnAssetSaved.Broadcast(World);
            ImGuiX::Notifications::NotifySuccess("Successfully saved world: \"{0}\"", World->GetName().c_str());
        }
        else
        {
            ImGuiX::Notifications::NotifyError("Failed to save world: \"{0}\"", World->GetName().c_str());
        }
    }

    void FWorldEditorTool::PushSaveAsAssetModal()
    {
        ToolContext->PushModal("Save World As Asset", ImVec2(550.0f, 240.0f), [this]() -> bool
        {
            static FFixedString NameBuffer;
            static FFixedString DirBuffer;
            static FFixedString ErrorMessage;

            if (ImGui::IsWindowAppearing())
            {
                NameBuffer = "NewWorld";
                DirBuffer = "/Game/Content";
                ErrorMessage.clear();
            }

            ImGui::TextUnformatted("This world is not saved as an asset yet.");
            ImGui::TextUnformatted("Pick a name and content folder to save it.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextUnformatted("Folder");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##Folder", DirBuffer.data(), DirBuffer.max_size());

            ImGui::Spacing();
            ImGui::TextUnformatted("Name");
            ImGui::SetNextItemWidth(-1.0f);
            const bool bEnter = ImGui::InputText("##Name", NameBuffer.data(), NameBuffer.max_size(), ImGuiInputTextFlags_EnterReturnsTrue);

            if (!ErrorMessage.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", ErrorMessage.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            constexpr float ButtonWidth = 110.0f;
            const float AvailWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX((AvailWidth - ButtonWidth * 2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

            const bool bConfirm = ImGui::Button("Save", ImVec2(ButtonWidth, 0.0f)) || bEnter;
            ImGui::SameLine();
            const bool bCancel = ImGui::Button("Cancel", ImVec2(ButtonWidth, 0.0f));

            if (bCancel)
            {
                return true;
            }

            if (!bConfirm)
            {
                return false;
            }

            if (NameBuffer.data()[0] == '\0')
            {
                ErrorMessage = "Name cannot be empty.";
                return false;
            }

            if (DirBuffer.data()[0] == '\0')
            {
                ErrorMessage = "Folder cannot be empty.";
                return false;
            }

            FFixedString Path = Paths::Combine(FStringView(DirBuffer.c_str()), FStringView(NameBuffer.c_str()));
            Path = VFS::ResolveToVirtualPath(Path);
            if (Path.empty() || Path.front() != '/')
            {
                ErrorMessage = "Folder must resolve to a virtual path under a mounted alias (e.g. /Game).";
                return false;
            }
            CPackage::AddPackageExt(Path);
            Path = VFS::MakeUniqueFilePath(Path);

            FFixedString SafePath = SanitizeObjectName(Path);
            if (FindObject<CPackage>(SafePath) != nullptr)
            {
                ErrorMessage = "A package with that name is already loaded.";
                return false;
            }

            CPackage* NewPackage = CPackage::CreatePackage(SafePath);
            if (NewPackage == nullptr)
            {
                ErrorMessage = "Failed to create package.";
                return false;
            }

            FStringView FileName = VFS::FileName(Path, true);
            World->Rename(FName(FileName), NewPackage);
            World->SetFlag(OF_Public);

            FObjectExport& Export = NewPackage->ExportTable.emplace_back();
            Export.ObjectGUID = World->GetGUID();
            Export.ObjectName = World->GetName();
            Export.ClassName = World->GetClass()->GetName();
            Export.Offset = 0;
            Export.Size = 0;
            Export.Object = World.Get();

            if (ShouldGenerateThumbnailOnSave())
            {
                GenerateThumbnail(NewPackage);
            }

            if (CPackage::SavePackage(NewPackage, Path))
            {
                FAssetRegistry::Get().AssetCreated(World);
                ImGuiX::Notifications::NotifySuccess("Saved world: \"{0}\"", Path);
                return true;
            }

            ErrorMessage = "Failed to save package to disk.";
            return false;
        });
    }

    namespace
    {
        // Includes a precomputed entity count, so the modal shows exactly what is being captured.
        struct FCreatePrefabRequest
        {
            TVector<ECS::FEntity> Roots;
            FVector3 Pivot;
            FFixedString DefaultName;
            uint32 TotalEntityCount;
        };

        FCreatePrefabRequest BuildCreatePrefabRequest(ECS::FRegistry& Registry, TVector<ECS::FEntity> InitialRoots)
        {
            FCreatePrefabRequest Out;
            Out.Roots = std::move(InitialRoots);
            Out.Pivot = FVector3(0.0f);
            Out.TotalEntityCount = 0;

            for (ECS::FEntity Entity : Out.Roots)
            {
                Out.Pivot += Registry.Get<STransformComponent>(Entity).GetWorldLocation();
                Out.TotalEntityCount += 1;
                ECS::Utils::ForEachDescendant(Registry, Entity, [&](ECS::FEntity)
                {
                    Out.TotalEntityCount += 1;
                });
            }
            if (!Out.Roots.empty())
            {
                Out.Pivot /= static_cast<float>(Out.Roots.size());
            }

            Out.DefaultName = "NewPrefab";
            if (Out.Roots.size() == 1)
            {
                if (const SNameComponent* NameComp = Registry.TryGet<SNameComponent>(Out.Roots[0]))
                {
                    Out.DefaultName = NameComp->Name.c_str();
                }
            }
            return Out;
        }
    }

    void FWorldEditorTool::PushCreatePrefabFromSelectionModal()
    {
        if (World == nullptr || World->IsSimulating())
        {
            ImGuiX::Notifications::NotifyWarning("Cannot create a prefab while simulating.");
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        // Drops invalid handles and prefab-instance children whose hierarchy is locked.
        THashSet<ECS::FEntity> Filtered;
        for (ECS::FEntity Entity : SelectedEntities)
        {
            if (Registry.IsValid(Entity) && !IsLockedPrefabChild(Registry, Entity))
            {
                Filtered.insert(Entity);
            }
        }

        if (Filtered.empty())
        {
            ImGuiX::Notifications::NotifyWarning("Select an entity in the world before creating a prefab.");
            return;
        }

        // If any ancestor is also selected, the descent happens through the parent instead.
        TVector<ECS::FEntity> Roots;
        Roots.reserve(Filtered.size());
        for (ECS::FEntity Entity : Filtered)
        {
            const FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Entity);
            ECS::FEntity Walk = Rel ? Rel->Parent : ECS::NullEntity;
            bool bAncestorInSet = false;
            while (Walk != ECS::NullEntity)
            {
                if (Filtered.find(Walk) != Filtered.end())
                {
                    bAncestorInSet = true;
                    break;
                }
                const FRelationshipComponent* WalkRel = Registry.TryGet<FRelationshipComponent>(Walk);
                Walk = WalkRel ? WalkRel->Parent : ECS::NullEntity;
            }
            if (!bAncestorInSet)
            {
                Roots.push_back(Entity);
            }
        }

        FCreatePrefabRequest Req = BuildCreatePrefabRequest(Registry, std::move(Roots));

        ToolContext->PushModal("Create Prefab From Selection", ImVec2(560.0f, 290.0f),
            [this, Req = std::move(Req)]() -> bool
        {
            static FFixedString NameBuffer;
            static FFixedString DirBuffer;
            static FFixedString ErrorMessage;

            if (ImGui::IsWindowAppearing())
            {
                NameBuffer = Req.DefaultName;
                DirBuffer = "/Game/Content";
                ErrorMessage.clear();
            }

            ImGui::TextUnformatted("Save the selection as a reusable prefab asset.");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Capturing %u entities total (%zu top-level + descendants).",
                Req.TotalEntityCount, Req.Roots.size());

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextUnformatted("Folder");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##Folder", DirBuffer.data(), DirBuffer.max_size());

            ImGui::Spacing();
            ImGui::TextUnformatted("Name");
            ImGui::SetNextItemWidth(-1.0f);
            const bool bEnter = ImGui::InputText("##Name", NameBuffer.data(), NameBuffer.max_size(), ImGuiInputTextFlags_EnterReturnsTrue);

            if (!ErrorMessage.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", ErrorMessage.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            constexpr float ButtonWidth = 110.0f;
            const float AvailWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX((AvailWidth - ButtonWidth * 2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

            const bool bConfirm = ImGui::Button("Create", ImVec2(ButtonWidth, 0.0f)) || bEnter;
            ImGui::SameLine();
            const bool bCancel = ImGui::Button("Cancel", ImVec2(ButtonWidth, 0.0f));

            if (bCancel) return true;
            if (!bConfirm) return false;

            if (NameBuffer.data()[0] == '\0') { ErrorMessage = "Name cannot be empty."; return false; }
            if (DirBuffer.data()[0]  == '\0') { ErrorMessage = "Folder cannot be empty."; return false; }

            ECS::FRegistry& WorkingRegistry = ECS::GetWorldRegistry(*World);
            for (ECS::FEntity Entity : Req.Roots)
            {
                if (!WorkingRegistry.IsValid(Entity))
                {
                    ErrorMessage = "Selection changed; cannot capture missing entities.";
                    return false;
                }
            }

            FFixedString Path = Paths::Combine(FStringView(DirBuffer.c_str()), FStringView(NameBuffer.c_str()));
            Path = VFS::ResolveToVirtualPath(Path);
            if (Path.empty() || Path.front() != '/')
            {
                ErrorMessage = "Folder must resolve to a virtual path under a mounted alias (e.g. /Game).";
                return false;
            }
            CPackage::AddPackageExt(Path);
            Path = VFS::MakeUniqueFilePath(Path);

            FFixedString SafePath = SanitizeObjectName(Path);
            if (FindObject<CPackage>(SafePath) != nullptr)
            {
                ErrorMessage = "A package with that name is already loaded.";
                return false;
            }

            CPackage* NewPackage = CPackage::CreatePackage(SafePath);
            if (NewPackage == nullptr) { ErrorMessage = "Failed to create package."; return false; }

            const FStringView FileName = VFS::FileName(Path, true);
            CPrefab* Prefab = NewObject<CPrefab>(NewPackage, FName(FileName));
            if (Prefab == nullptr) { ErrorMessage = "Failed to create prefab object."; return false; }
            Prefab->SetFlag(OF_Public);

            // A scratch parent at the pivot, reparented world-preserving, then captured and restored.
            ECS::FEntity CaptureRoot = ECS::NullEntity;
            ECS::FEntity ScratchRoot = ECS::NullEntity;
            TVector<ECS::FEntity> OriginalParents;

            if (Req.Roots.size() == 1)
            {
                CaptureRoot = Req.Roots[0];
            }
            else
            {
                FTransform PivotTransform;
                PivotTransform.SetLocation(Req.Pivot);
                ScratchRoot = World->ConstructEntity(FName(FileName), PivotTransform);
                if (ScratchRoot == ECS::NullEntity)
                {
                    ErrorMessage = "Failed to create scratch parent.";
                    return false;
                }

                OriginalParents.reserve(Req.Roots.size());
                for (ECS::FEntity Entity : Req.Roots)
                {
                    const FRelationshipComponent* Rel = WorkingRegistry.TryGet<FRelationshipComponent>(Entity);
                    OriginalParents.push_back(Rel ? Rel->Parent : ECS::NullEntity);
                    ECS::Utils::ReparentEntity(WorkingRegistry, Entity, ScratchRoot);
                }
                CaptureRoot = ScratchRoot;
            }

            Prefab->CaptureFromWorld(World, CaptureRoot);

            // Anchor the captured root at origin so the prefab opens centered in its editor.
            Prefab->Registry.ForEachEntity([&](ECS::FEntity E)
            {
                const FRelationshipComponent* Rel = Prefab->Registry.TryGet<FRelationshipComponent>(E);
                if (Rel != nullptr && Rel->Parent != ECS::NullEntity)
                {
                    return;
                }
                if (STransformComponent* Tx = Prefab->Registry.TryGet<STransformComponent>(E))
                {
                    Tx->SetLocalTransform(FTransform());
                }
            });

            // Detaches top-level entities back to their original parents and drops the scratch.
            if (ScratchRoot != ECS::NullEntity)
            {
                for (size_t i = 0; i < Req.Roots.size(); ++i)
                {
                    if (OriginalParents[i] == ECS::NullEntity)
                    {
                        ECS::Utils::RemoveFromParent(WorkingRegistry, Req.Roots[i]);
                    }
                    else
                    {
                        ECS::Utils::ReparentEntity(WorkingRegistry, Req.Roots[i], OriginalParents[i]);
                    }
                }
                World->DestroyEntity(ScratchRoot);
            }

            FObjectExport& Export = NewPackage->ExportTable.emplace_back();
            Export.ObjectGUID = Prefab->GetGUID();
            Export.ObjectName = Prefab->GetName();
            Export.ClassName = Prefab->GetClass()->GetName();
            Export.Offset = 0;
            Export.Size = 0;
            Export.Object = Prefab;

            if (CPackage::SavePackage(NewPackage, Path))
            {
                FAssetRegistry::Get().AssetCreated(Prefab);
                ImGuiX::Notifications::NotifySuccess("Created prefab: \"{0}\"", Path);
                return true;
            }

            ErrorMessage = "Failed to save prefab to disk.";
            return false;
        });
    }

    void FWorldEditorTool::PushCreatePrefabModalForEntity(ECS::FEntity Entity)
    {
        if (World == nullptr || World->IsSimulating())
        {
            ImGuiX::Notifications::NotifyWarning("Cannot create a prefab while simulating.");
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (!Registry.IsValid(Entity) || IsLockedPrefabChild(Registry, Entity))
        {
            ImGuiX::Notifications::NotifyWarning("Cannot create a prefab from this entity.");
            return;
        }

        // Fake single-entity selection to reuse PushCreatePrefabFromSelectionModal without a more invasive hook.
        const THashSet<ECS::FEntity> SavedSelection = SelectedEntities;
        SelectedEntities.clear();
        SelectedEntities.insert(Entity);
        PushCreatePrefabFromSelectionModal();
        SelectedEntities = SavedSelection;
    }

    void FWorldEditorTool::ResyncPrefabInstance(ECS::FEntity InstanceRoot)
    {
        if (World == nullptr || World->IsSimulating())
        {
            ImGuiX::Notifications::NotifyWarning("Cannot resync a prefab while simulating.");
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        const SPrefabInstanceComponent* Instance = Registry.TryGet<SPrefabInstanceComponent>(InstanceRoot);
        if (Instance == nullptr || !Instance->bIsRoot)
        {
            return;
        }

        // RefreshInstance rewrites this entity's components, so the pointer above dangles once it runs.
        CPrefab* Source = Instance->SourcePrefab.Get();
        if (Source == nullptr)
        {
            ImGuiX::Notifications::NotifyWarning("This instance has no source prefab to resync from.");
            return;
        }
        const FName PrefabName = Source->GetName();

        // The refresh adds, destroys and reparents, so this needs the whole-registry snapshot.
        BeginTransaction();
        Source->RefreshInstance(World, InstanceRoot);
        EndTransaction("Resync Prefab");

        // A pruned node may have been selected, and every details-panel component pointer is stale.
        ResyncSelectionFromRegistry();
        bDetailsDirty = true;
        OutlinerListView.MarkTreeDirty();

        ImGuiX::Notifications::NotifySuccess("Resynced to prefab: \"{0}\"", PrefabName.c_str());
    }

    void FWorldEditorTool::DetachPrefabInstance(ECS::FEntity InstanceRoot)
    {
        BeginTransaction();
        if (CPrefab::DetachInstance(World, InstanceRoot))
        {
            EndTransaction("Detach from Prefab");
            OutlinerListView.MarkTreeDirty();
        }
        else
        {
            AbortTransaction();
        }
    }

    bool FWorldEditorTool::IsAssetEditorTool() const
    {
        return true;
    }

    void FWorldEditorTool::NotifyPlayInEditorStart()
    {
        bGamePreviewRunning = true;
    }

    void FWorldEditorTool::NotifyPlayInEditorStop()
    {
         bGamePreviewRunning = false;
    }

    void FWorldEditorTool::SetWorld(CWorld* InWorld)
    {
        // Stop inspecting any foreign world (while its registry is still alive) before swapping the document.
        if (IsInspectingForeignWorld())
        {
            SetObservedWorld(nullptr);
        }

        // RebindRegistryObservers re-binds to the new world, and tags clear on the current one.
        UnbindRegistryObservers();
        if (World)
        {
            ECS::FRegistry& OldRegistry = ECS::GetWorldRegistry(*World);
            OldRegistry.ClearComponent<FSelectedInEditorComponent>();
            OldRegistry.ClearComponent<FLastSelectedTag>();
        }

        // Property tables hold raw component pointers and the selection cache holds old handles.
        PropertyTables.clear();
        SelectedEntities.clear();
        LastSelectedEntity = ECS::NullEntity;
        DetailsEntity = ECS::NullEntity;
        bDetailsDirty = true;

        FEditorTool::SetWorld(InWorld);

        WorldSettingsPropertyTable = MakeUnique<FPropertyTable>(&World->GetDefaultWorldSettings(), SDefaultWorldSettings::StaticStruct());

        RebindRegistryObservers();
        OutlinerListView.MarkTreeDirty();
    }

    void FWorldEditorTool::OnEntityDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        // During a restore, entities are destroyed then recreated at the same handles; skip pruning so caches survive (OnPostUndoRedo rebuilds).
        if (bRestoringTransaction)
        {
            return;
        }

        // Cached property tables are invalidated, since their component pointers are about to dangle.
        if (SelectedEntities.find(Entity) != SelectedEntities.end())
        {
            SelectedEntities.erase(Entity);
        }

        if (LastSelectedEntity == Entity)
        {
            ECS::FEntity NewLast = ECS::NullEntity;
            for (ECS::FEntity Candidate : SelectedEntities)
            {
                if (Registry.IsValid(Candidate))
                {
                    NewLast = Candidate;
                    break;
                }
            }
            LastSelectedEntity = NewLast;
            bDetailsDirty = true;
        }

        if (DetailsEntity == Entity)
        {
            PropertyTables.clear();
            DetailsEntity = ECS::NullEntity;
            bDetailsDirty = true;
        }
    }

    void FWorldEditorTool::DrawSimulationControls(float ButtonSize)
    {
        const ImVec2 BtnSize = ImVec2(ButtonSize, ButtonSize);
        
        if (!bGamePreviewRunning)
        {
            if (!bSimulatingWorld)
            {
                // An unsaved transient world has no package, so disable both rather than let StartPIE return null.
                const bool bCanSimulate = World != nullptr && World->GetPackage() != nullptr;

                ImGui::BeginDisabled(!bCanSimulate);

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.3f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.8f, 0.4f, 1.0f));
                if (ImGuiX::IconButton(LE_ICON_PLAY, "##PlayBtn", 0xFFFFFFFF, BtnSize))
                {
                    SetWorldPlayInEditor(true);
                }
                ImGui::PopStyleColor(2);

                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip(bCanSimulate ? "Play (Start game preview)" : "Save the world before playing");
                }

                ImGui::SameLine();

                if (ImGuiX::IconButton(LE_ICON_COG_BOX, "##SimulateBtn", 0xFFFFFFFF, BtnSize))
                {
                    SetWorldNewSimulate(true);
                }

                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip(bCanSimulate ? "Simulate (Run physics without gameplay)" : "Save the world before simulating");
                }

                ImGui::EndDisabled();

                ImGui::SameLine(0.0f, 8.0f);
                if (ImGui::ArrowButton("##PlaySettings", ImGuiDir_Down))
                {
                    ImGui::OpenPopup("PlaySettingsPopup");
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGui::SetTooltip("Play settings (players, net mode)");
                }
                DrawPlaySettingsPopup();

                // Compact reminder of the current multiplayer config.
                if (PlaySettings.NumPlayers > 1)
                {
                    ImGui::SameLine(0.0f, 6.0f);
                    ImGui::TextDisabled("%dP", PlaySettings.NumPlayers);
                }
            }
            else
            {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    SetWorldNewSimulate(false);
                }

                DrawPauseResumeButton(BtnSize);

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.5f, 0.1f, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
                if (ImGuiX::IconButton(LE_ICON_COG_BOX, "##SimulateActiveBtn", 0xFFFFFFFF, BtnSize))
                {
                    SetWorldNewSimulate(false);
                }
                ImGui::PopStyleColor(2);
                
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGui::SetTooltip("Stop Simulation (ESC)");
                }
            }
        }
        else
        {
            DrawPauseResumeButton(BtnSize);
            DrawEjectButton(BtnSize);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 0.75f));

            if (ImGuiX::IconButton(LE_ICON_STOP, "##StopBtn", 0xFFFFFFFF, BtnSize, true))
            {
                SetWorldPlayInEditor(false);
            }
            ImGui::PopStyleColor(2);
            
            
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGui::SetTooltip("Stop Game Preview");
            }
            
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                SetWorldPlayInEditor(false);
            }
        }
    }

    void FWorldEditorTool::DrawPauseResumeButton(const ImVec2& ButtonSize)
    {
        const bool bPaused = IsPlaySessionPaused();

        // The button always advertises what the NEXT click does, matching Play and Stop beside it.
        ImGui::PushStyleColor(ImGuiCol_Button,        bPaused ? ImVec4(0.2f, 0.7f, 0.3f, 0.8f) : ImVec4(0.85f, 0.65f, 0.15f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bPaused ? ImVec4(0.3f, 0.8f, 0.4f, 1.0f) : ImVec4(0.95f, 0.75f, 0.25f, 1.0f));
        if (ImGuiX::IconButton(bPaused ? LE_ICON_PLAY : LE_ICON_PAUSE, "##PauseResumeBtn", 0xFFFFFFFF, ButtonSize))
        {
            SetPlaySessionPaused(!bPaused);
        }
        ImGui::PopStyleColor(2);

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip(bPaused
                ? "Resume"
                : "Pause (freezes systems and physics; the viewport keeps rendering and you can still select/inspect)");
        }

        ImGui::SameLine();
    }

    bool FWorldEditorTool::IsPlaySessionPaused() const
    {
        // Editor worlds are permanently paused, hence the play-state gate on this session's own pause.
        return (bGamePreviewRunning || bSimulatingWorld) && World != nullptr && World->IsPaused();
    }

    void FWorldEditorTool::SetPlaySessionPaused(bool bPause)
    {
        if (GWorldManager == nullptr)
        {
            return;
        }

        // Multiplayer PIE spawns a world per player, and pausing only ours leaves the rest simulating.
        for (const TUniquePtr<FWorldContext>& Context : GWorldManager->GetContexts())
        {
            CWorld* Candidate = Context ? Context->World.Get() : nullptr;
            if (Candidate == nullptr)
            {
                continue;
            }

            const EWorldType Type = Candidate->GetWorldType();
            if (Type == EWorldType::Game || Type == EWorldType::Simulation)
            {
                Candidate->SetPaused(bPause);
            }
        }
    }

    void FWorldEditorTool::DrawEjectButton(const ImVec2& ButtonSize)
    {
        const bool bEjected = IsEjectedFromPlay();

        ImGui::PushStyleColor(ImGuiCol_Button,        bEjected ? ImVec4(0.25f, 0.45f, 0.85f, 0.85f) : ImVec4(0.32f, 0.36f, 0.44f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bEjected ? ImVec4(0.35f, 0.55f, 0.95f, 1.0f)  : ImVec4(0.42f, 0.47f, 0.57f, 1.0f));
        if (ImGuiX::IconButton(bEjected ? LE_ICON_CONTROLLER : LE_ICON_EJECT, "##EjectBtn", 0xFFFFFFFF, ButtonSize))
        {
            SetEjectedFromPlay(!bEjected);
        }
        ImGui::PopStyleColor(2);

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip(bEjected
                ? "Possess (F8): hand the view and input back to the game camera"
                : "Eject (F8): detach to a free-fly editor camera; the game keeps running");
        }

        ImGui::SameLine();
    }

    void FWorldEditorTool::SetEjectedFromPlay(bool bEject)
    {
        if (World == nullptr || !bGamePreviewRunning || bEject == bEjectedFromPlay)
        {
            return;
        }

        if (bEject)
        {
            if (EditorEntity != ECS::NullEntity)
            {
                // A play session has no editor entity, so one here means another path already built a camera.
                LOG_WARN("Eject: the PIE world already has an editor camera; ignoring.");
                return;
            }

            // Captured BEFORE SetupWorldForTool, whose last step makes the editor entity active.
            PossessedCameraEntity = World->GetActiveCameraEntity();

            FTransform CameraPose;
            bool bHasPose = false;
            if (PossessedCameraEntity != ECS::NullEntity && World->IsValidEntity(PossessedCameraEntity)
                && World->HasComponent<STransformComponent>(PossessedCameraEntity))
            {
                CameraPose = World->GetComponent<STransformComponent>(PossessedCameraEntity).GetWorldTransform();
                bHasPose = true;
            }

            // TickEditorCamera has no game-world gate, so it drives the flycam with no further plumbing.
            SetupWorldForTool();

            // Start from the game camera's pose; the default pose would drop you at the world origin.
            if (bHasPose && EditorEntity != ECS::NullEntity && World->IsValidEntity(EditorEntity))
            {
                World->GetComponent<STransformComponent>(EditorEntity).SetLocalTransform(CameraPose);
                World->EmplaceComponent<FNeedsTransformUpdate>(EditorEntity);
            }

            // Game view hides the overlays that make selecting a live entity usable.
            bRestoreGameViewOnPossess = bGameViewMode;
            if (bGameViewMode)
            {
                ToggleGameViewMode();
            }

            // Set last, since HasEditorCameraControl flips with it and the refocus handler reads it next draw.
            bEjectedFromPlay = true;
            SetInputFocus(EInputFocus::Editor);
        }
        else
        {
            // Re-activated first, so the world is never left with a resolved view on a dead entity.
            if (PossessedCameraEntity != ECS::NullEntity && World->IsValidEntity(PossessedCameraEntity))
            {
                World->SetActiveCamera(PossessedCameraEntity);
            }
            else
            {
                ImGuiX::Notifications::NotifyWarning("The camera that was possessed is gone; the game will pick its own.");
            }
            PossessedCameraEntity = ECS::NullEntity;

            if (EditorEntity != ECS::NullEntity && World->IsValidEntity(EditorEntity))
            {
                World->DestroyEntity(EditorEntity);
            }
            EditorEntity = ECS::NullEntity;

            if (bRestoreGameViewOnPossess && !bGameViewMode)
            {
                ToggleGameViewMode();
            }
            bRestoreGameViewOnPossess = false;

            bEjectedFromPlay = false;
            SetInputFocus(EInputFocus::Game);
        }
    }

    void FWorldEditorTool::TickEjectState()
    {
        if (!bEjectedFromPlay)
        {
            return;
        }

        // Deliberately NOT SetEjectedFromPlay(false), since its possess path would touch the dead world.
        if (!bGamePreviewRunning || World == nullptr
            || EditorEntity == ECS::NullEntity || !World->IsValidEntity(EditorEntity))
        {
            bEjectedFromPlay = false;
            PossessedCameraEntity = ECS::NullEntity;
            bRestoreGameViewOnPossess = false;
        }
    }

    void FWorldEditorTool::StopAllSimulations()
    {
        SetWorldNewSimulate(false);
        SetWorldPlayInEditor(false);
    }

    void FWorldEditorTool::DrawPlaySettingsPopup()
    {
        if (!ImGui::BeginPopup("PlaySettingsPopup"))
        {
            return;
        }

        ImGui::TextUnformatted("Play In Editor");
        ImGui::Separator();

        int32 Num = PlaySettings.NumPlayers;
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::SliderInt("Players", &Num, 1, MaxPlayers))
        {
            PlaySettings.NumPlayers = (Num < 1) ? 1 : (Num > MaxPlayers ? MaxPlayers : Num);
        }

        // Order must match ENetMode, so Standalone, Client, ListenServer, then DedicatedServer.
        static const char* const NetModeNames[] = { "Standalone", "Client", "Listen Server", "Dedicated Server" };
        int32 ModeIdx = (int32)PlaySettings.NetMode;
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::Combo("Net Mode", &ModeIdx, NetModeNames, IM_ARRAYSIZE(NetModeNames)))
        {
            PlaySettings.NetMode = (ENetMode)ModeIdx;
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Standalone");
        {
            const bool bBuilding = FStandaloneLauncher::IsBuilding();

            ImGui::Checkbox("Current map", &PlaySettings.bStandaloneCurrentMap);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGui::SetTooltip("Off launches the project's configured startup map instead.");
            }

            ImGui::Checkbox("Save modified assets", &PlaySettings.bStandaloneSaveFirst);
            ImGui::Checkbox("Build game binaries if missing", &PlaySettings.bStandaloneBuild);

            ImGui::BeginDisabled(bBuilding);
            if (ImGui::Button(bBuilding ? "Building..." : "Launch Standalone", ImVec2(170.0f, 0.0f)))
            {
                LaunchStandalone();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();

            if (!bBuilding && !FStandaloneLauncher::HasGameBinaries())
            {
                ImGui::TextDisabled("Game binaries not built yet.");
            }
        }

        // Bound straight to the CVars the transport reads, so dragging these takes effect live.
        ImGui::Separator();
        ImGui::TextUnformatted("Network Simulation");
        {
            FConsoleRegistry& Console = FConsoleRegistry::Get();
            ImGui::BeginDisabled(PlaySettings.NetMode == ENetMode::Standalone);

            int32 LatencyMs = Console.GetAs<int32>("Net.Sim.LatencyMs");
            ImGui::SetNextItemWidth(170.0f);
            if (ImGui::SliderInt("Latency (ms)", &LatencyMs, 0, 500))
            {
                Console.SetAs<int32>("Net.Sim.LatencyMs", LatencyMs < 0 ? 0 : LatencyMs);
            }

            float LossPct = Console.GetAs<float>("Net.Sim.PacketLossPct");
            ImGui::SetNextItemWidth(170.0f);
            if (ImGui::SliderFloat("Packet Loss", &LossPct, 0.0f, 100.0f, "%.0f%%"))
            {
                Console.SetAs<float>("Net.Sim.PacketLossPct", LossPct < 0.0f ? 0.0f : (LossPct > 100.0f ? 100.0f : LossPct));
            }

            ImGui::EndDisabled();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Players 2+ open preview windows in this process.");

        ImGui::EndPopup();
    }

    void FWorldEditorTool::LaunchStandalone()
    {
        // The game process reads packages off disk, so anything unsaved here is invisible to it.
        if (PlaySettings.bStandaloneSaveFirst && ToolContext != nullptr)
        {
            ToolContext->SaveAllDirtyPackages();
        }

        FStandaloneLaunchOptions Options;
        Options.bBuildIfMissing = PlaySettings.bStandaloneBuild;

        if (PlaySettings.bStandaloneCurrentMap && World != nullptr && World->GetPackage() != nullptr)
        {
            const FFixedString MapPath = World->GetPackage()->GetPackagePath();
            Options.MapPath.assign(MapPath.c_str(), MapPath.size());
        }

        FStandaloneLauncher::Request(Options);
    }

    ENetMode FWorldEditorTool::ResolvePlayerNetMode(int32 PlayerIndex) const
    {
        switch (PlaySettings.NetMode)
        {
        case ENetMode::Standalone:      return ENetMode::Standalone;
        case ENetMode::Client:          return ENetMode::Client;
        case ENetMode::ListenServer:    return PlayerIndex == 0 ? ENetMode::ListenServer : ENetMode::Client;
        // The dedicated server is a separate hidden world, so every visible player is a client.
        case ENetMode::DedicatedServer: return ENetMode::Client;
        }
        return ENetMode::Standalone;
    }

    void FWorldEditorTool::OnPostUndoRedo()
    {
        // The true-restore preserves handles but drops the (non-serialized) selection tags; re-stamp then rebuild the cache.
        ReapplySelectionTags();
        ResyncSelectionFromRegistry();

        // The restore reallocated component storage, so the details panel's cached pointers are stale.
        bDetailsDirty = true;

        // Outliner topology may have changed; force a rebuild.
        OutlinerListView.MarkTreeDirty();

        // Terrain GPU mirrors are transient, so a restore needs a full re-upload and chunk rebuild.
        if (World)
        {
            auto TerrainView = ECS::GetWorldRegistry(*World).View<STerrainComponent>();
            for (ECS::FEntity Entity : TerrainView)
            {
                FTerrainCPUState& State = ECS::GetWorldRegistry(*World).Get<STerrainComponent>(Entity).CPUState;
                State.bFullHeightmapDirty = true;
                State.bFullWeightsDirty   = true;
                State.bChunksDirty        = true;
            }
        }
    }

    bool FWorldEditorTool::AllowsUndoRedo() const
    {
        // Not during play, since World then points at the transient world, not the edited source.
        return World != nullptr && World->GetWorldType() == EWorldType::Editor;
    }

    namespace
    {
        FORCEINLINE void SetTreeNodeSelected(FTreeListView& Tree, FTreeNodeID Node, bool bSelected)
        {
            if (Node.IsValid() && Tree.IsValid(Node))
            {
                Tree.Get<FTreeNodeState>(Node).bSelected = bSelected;
            }
        }
    }

    void FWorldEditorTool::GetDefaultCameraPose(FVector3& OutLocation, FVector3& OutTarget) const
    {
        EditorEntityUtils::GetDefaultScenePreviewPose(OutLocation, OutTarget);
    }

    void FWorldEditorTool::UnbindRegistryObservers()
    {
        if (ObservedRegistry == nullptr)
        {
            return;
        }
        ObservedRegistry->OnEntityCreated().Disconnect<&FWorldEditorTool::OnEntityCreated>(this);
        ObservedRegistry->OnEntityDestroyed().Disconnect<&FWorldEditorTool::OnEntityDestroyed>(this);
        ObservedRegistry->GetSignals<SNameComponent>().OnConstruct.Disconnect<&FSceneEditorTool::OnOutlinerEntityConstructed>(this);
        ObservedRegistry->GetSignals<SNameComponent>().OnDestroy.Disconnect<&FSceneEditorTool::OnOutlinerEntityDestroyed>(this);
        ObservedRegistry->GetSignals<SEntityScriptComponent>().OnConstruct.Disconnect<&FWorldEditorTool::OnEntityScriptChanged>(this);
        ObservedRegistry->GetSignals<SEntityScriptComponent>().OnDestroy.Disconnect<&FWorldEditorTool::OnEntityScriptChanged>(this);
        ObservedRegistry = nullptr;
    }

    void FWorldEditorTool::RebindRegistryObservers()
    {
        UnbindRegistryObservers();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*GetObservedWorld());
        Registry.OnEntityCreated().Connect<&FWorldEditorTool::OnEntityCreated>(this);
        Registry.OnEntityDestroyed().Connect<&FWorldEditorTool::OnEntityDestroyed>(this);
        Registry.GetSignals<SNameComponent>().OnConstruct.Connect<&FSceneEditorTool::OnOutlinerEntityConstructed>(this);
        Registry.GetSignals<SNameComponent>().OnDestroy.Connect<&FSceneEditorTool::OnOutlinerEntityDestroyed>(this);
        Registry.GetSignals<SEntityScriptComponent>().OnConstruct.Connect<&FWorldEditorTool::OnEntityScriptChanged>(this);
        Registry.GetSignals<SEntityScriptComponent>().OnDestroy.Connect<&FWorldEditorTool::OnEntityScriptChanged>(this);
        ObservedRegistry = &Registry;

        OutlinerListView.MarkTreeDirty();
    }

    void FWorldEditorTool::SetObservedWorld(CWorld* NewWorld)
    {
        // A sentinel, where observing the tool's own world means following World with a null observed.
        CWorld* Target = (NewWorld == World.Get()) ? nullptr : NewWorld;
        if (Target == ObservedWorld)
        {
            return;
        }

        // Foreign worlds are reset before teardown, and Update validates the dead-world case.
        UnbindRegistryObservers();

        // Old entity handles and component pointers mean nothing against the new registry.
        PropertyTables.clear();
        SelectedEntities.clear();
        LastSelectedEntity = ECS::NullEntity;
        DetailsEntity = ECS::NullEntity;
        bDetailsDirty = true;

        ObservedWorld = Target;

        OutlinerListView.ClearTree();
        OutlinerListView.MarkTreeDirty();
        EntityToTreeNode.clear();
        PendingOutlinerAdds.clear();
        PendingExpanderRefresh.clear();

        RebindRegistryObservers();      // binds to GetObservedWorld()
        ResyncSelectionFromRegistry();  // adopt any selection tags already on the observed world
    }

    void FWorldEditorTool::DrawOutlinerWorldSelector()
    {
        if (GWorldManager == nullptr)
        {
            return;
        }

        // Other tools' preview worlds are excluded, so the selector only appears for play.
        TVector<FWorldContext*> Candidates;
        for (const TUniquePtr<FWorldContext>& Ctx : GWorldManager->GetContexts())
        {
            CWorld* CtxWorld = Ctx->World.Get();
            if (CtxWorld == nullptr)
            {
                continue;
            }
            if (CtxWorld == World.Get() || Ctx->Type == EWorldType::Game || Ctx->Type == EWorldType::Simulation)
            {
                Candidates.push_back(Ctx.get());
            }
        }

        if (Candidates.size() <= 1)
        {
            // Nothing to switch between (normal single-world editing).
            return;
        }

        auto NetModeTag = [](ENetMode Mode) -> const char*
        {
            switch (Mode)
            {
            case ENetMode::Standalone:      return "Standalone";
            case ENetMode::Client:          return "Client";
            case ENetMode::ListenServer:    return "Server (Listen)";
            case ENetMode::DedicatedServer: return "Server (Dedicated)";
            }
            return "Unknown";
        };

        auto WorldLabel = [&](const FWorldContext& Ctx) -> FString
        {
            FString Label = NetModeTag(Ctx.NetMode);
            if (!Ctx.MapPath.empty())
            {
                Label += " - ";
                Label += Ctx.MapPath;
            }
            if (Ctx.Type == EWorldType::Editor)
            {
                Label += " (editor)";
            }
            return Label;
        };

        CWorld* Observed = GetObservedWorld();
        FString Preview = "World: ";
        if (FWorldContext* ObservedCtx = GWorldManager->FindContext(Observed))
        {
            Preview += WorldLabel(*ObservedCtx);
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##WorldSelector", Preview.c_str()))
        {
            for (FWorldContext* Ctx : Candidates)
            {
                CWorld* CtxWorld = Ctx->World.Get();
                const bool bSelected = (CtxWorld == Observed);
                if (ImGui::Selectable(WorldLabel(*Ctx).c_str(), bSelected))
                {
                    SetObservedWorld(CtxWorld);
                }
                if (bSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        if (IsInspectingForeignWorld())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.2f, 1.0f));
            ImGui::TextWrapped(LE_ICON_EYE " Inspecting another world. The viewport still shows your own, so selection gizmos won't appear here.");
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
    }

    void FWorldEditorTool::OnWorldTraveled(CWorld* OldWorld, CWorld* NewWorld)
    {
        // Only react if Travel swapped the world this tool is displaying.
        if (OldWorld != World.Get() || NewWorld == nullptr)
        {
            return;
        }

        // Stop inspecting any foreign world before World changes underneath us (its registry is still alive here).
        if (IsInspectingForeignWorld())
        {
            SetObservedWorld(nullptr);
        }

        // Drop pointers into the torn-down world before rebinding.
        PropertyTables.clear();
        WorldSettingsPropertyTable.reset();

        // Old entity handles are meaningless against the new registry.
        SelectedEntities.clear();
        LastSelectedEntity = ECS::NullEntity;
        DetailsEntity = ECS::NullEntity;
        bDetailsDirty = true;

        EditorEntity = ECS::NullEntity;

        // ProxyWorld is untouched, so stopping play can still restore the editor's source map.
        RebindToWorld(NewWorld);

        WorldSettingsPropertyTable = MakeUnique<FPropertyTable>(&World->GetDefaultWorldSettings(), SDefaultWorldSettings::StaticStruct());

        OutlinerListView.ClearTree();
        OutlinerListView.MarkTreeDirty();
        EntityToTreeNode.clear();
        PendingOutlinerAdds.clear();
        PendingExpanderRefresh.clear();

        RebindRegistryObservers();

        // Simulate owns the editor entity, so rebuild it or simulate-exit dereferences a null handle.
        if (bSimulatingWorld)
        {
            SetupWorldForTool();
        }
    }

    void FWorldEditorTool::SetWorldPlayInEditor(bool bShouldPlay)
    {
        if (bShouldPlay == bGamePreviewRunning)
        {
            return;
        }

        // Stopping play tears those worlds down, which would leave our observers dangling.
        if (IsInspectingForeignWorld())
        {
            SetObservedWorld(nullptr);
        }

        if (bShouldPlay)
        {
            bGamePreviewRunning = true;
            bGameViewMode = true; // default to a clean game view on play; toggle off to see editor/debug overlays
            PropertyTables.clear();
            SelectedEntities.clear();
            LastSelectedEntity = ECS::NullEntity;
            DetailsEntity = ECS::NullEntity;
            bDetailsDirty = true;

            // Leftover tags on ProxyWorld confuse the outliner rebuild on PIE-exit.
            ECS::GetWorldRegistry(*World).ClearComponent<FSelectedInEditorComponent>();
            ECS::GetWorldRegistry(*World).ClearComponent<FLastSelectedTag>();

            World->SetActive(false);
            ProxyWorld = World;
            ProxyEditorEntity = EditorEntity;

            // Its net mode makes CreateRenderer a no-op, so it stays invisible and listens on loopback.
            if (PlaySettings.NetMode == ENetMode::DedicatedServer)
            {
                PIEDedicatedServerWorld = GWorldManager->StartPIE(ProxyWorld, EWorldType::Game, ENetMode::DedicatedServer);
                if (PIEDedicatedServerWorld == nullptr)
                {
                    LOG_WARN("Failed to start the dedicated-server world for PIE.");
                }
            }

            // StartPIE returns null when the world cannot be duplicated, so bail before rebinding to null.
            CWorld* PIEWorld = GWorldManager->StartPIE(ProxyWorld, EWorldType::Game, ResolvePlayerNetMode(0));
            if (PIEWorld == nullptr)
            {
                LOG_WARN("Cannot Play '{0}': world has no package (save the world before playing).", World->GetName().c_str());
                ImGuiX::Notifications::NotifyError("Cannot Play: save the world first.");
                if (PIEDedicatedServerWorld != nullptr)
                {
                    GWorldManager->StopPIE(PIEDedicatedServerWorld);
                    PIEDedicatedServerWorld = nullptr;
                }
                World->SetActive(true);
                ProxyWorld = nullptr;
                ProxyEditorEntity = ECS::NullEntity;
                bGamePreviewRunning = false;
                return;
            }
            RebindToWorld(PIEWorld);
            EditorEntity = ECS::NullEntity;

            WorldSettingsPropertyTable = MakeUnique<FPropertyTable>(&World->GetDefaultWorldSettings(), SDefaultWorldSettings::StaticStruct());

            OutlinerListView.ClearTree();
            OutlinerListView.MarkTreeDirty();

            RebindRegistryObservers();

            // Play starts in Game focus, so ImGui stands down and input goes to the game and its UI.
            SetInputFocus(EInputFocus::Game);

            // FEditorUI spawns the other players' PIE worlds and pop-ups through a deferred tool create.
            OnGamePreviewStartRequested.Broadcast();
            FCoreEditorDelegates::OnPIEBegin.Broadcast(PIEWorld);
        }
        else
        {
            // Broadcast while the PIE world is still live; the teardown below invalidates it.
            FCoreEditorDelegates::OnPIEEnd.Broadcast(World);

            // Tear down extra player preview worlds (FEditorUI) before the primary teardown below.
            OnGamePreviewStopRequested.Broadcast();

            // Tear down the hidden dedicated-server world, if one was spawned.
            if (PIEDedicatedServerWorld != nullptr)
            {
                GWorldManager->StopPIE(PIEDedicatedServerWorld);
                PIEDedicatedServerWorld = nullptr;
            }

            // Not via SetEjectedFromPlay, whose possess path would rebuild a camera in a dying world.
            bEjectedFromPlay = false;
            PossessedCameraEntity = ECS::NullEntity;
            bRestoreGameViewOnPossess = false;

            PropertyTables.clear();
            ResetSelectionState();
            DetailsEntity = ECS::NullEntity;
            bDetailsDirty = true;
            World->SetPaused(true);
            bGamePreviewRunning = false;
            bGameViewMode = false; // back to the editor view on stop

            // Hand input back to the editor on stop.
            SetInputFocus(EInputFocus::Editor);

            // SetupWorldForTool builds a fresh editor entity at origin, so stash the pre-Play camera pose.
            bool bHasSavedCamera = false;
            FTransform SavedCameraTransform;
            SCameraComponent SavedCamera;
            // Use ProxyEditorEntity (captured at PIE entry); EditorEntity may be null if Travel swapped worlds mid-PIE.
            if (ProxyEditorEntity != ECS::NullEntity && ECS::GetWorldRegistry(*ProxyWorld).IsValid(ProxyEditorEntity))
            {
                SavedCameraTransform = ECS::GetWorldRegistry(*ProxyWorld).Get<STransformComponent>(ProxyEditorEntity).GetWorldTransform();
                SavedCamera = ECS::GetWorldRegistry(*ProxyWorld).Get<SCameraComponent>(ProxyEditorEntity);
                bHasSavedCamera = true;

                ProxyWorld->DestroyEntity(ProxyEditorEntity);
            }
            ProxyEditorEntity = ECS::NullEntity;
            EditorEntity = ECS::NullEntity;

            SetWorld(ProxyWorld);
            ProxyWorld->SetActive(true);

            if (bHasSavedCamera && ECS::GetWorldRegistry(*World).IsValid(EditorEntity))
            {
                ECS::GetWorldRegistry(*World).Get<STransformComponent>(EditorEntity).SetLocalTransform(SavedCameraTransform);
                ECS::GetWorldRegistry(*World).Patch<SCameraComponent>(EditorEntity, [SavedCamera](SCameraComponent& Patch)
                {
                    Patch = SavedCamera;
                });
            }

            WorldSettingsPropertyTable = MakeUnique<FPropertyTable>(&World->GetDefaultWorldSettings(), SDefaultWorldSettings::StaticStruct());

            ProxyWorld = nullptr;

            OutlinerListView.ClearTree();
            OutlinerListView.MarkTreeDirty();

            RebindRegistryObservers();

            if (InputViewport)
            {
                // Activate editor viewport first so FInputProcessor routes the mode change against the right context.
                FInputViewportRegistry::Get().SetActiveViewport(InputViewport.get());

                InputViewport->GetContext().SetInputMode(EInputMode::Game);

                // Route through FInputProcessor so ImGuiConfigFlags_NoMouse clears; direct context-field set would leave ImGui ignoring the mouse.
                FInputProcessor::Get().SetMouseMode(EMouseMode::Normal);
            }
        }
    }

    void FWorldEditorTool::SetWorldNewSimulate(bool bShouldSimulate)
    {
        // Detach from any inspected foreign world (registry still alive) before simulate teardown/setup.
        if (IsInspectingForeignWorld())
        {
            SetObservedWorld(nullptr);
        }

        if (bShouldSimulate == bSimulatingWorld)
        {
            return;
        }

        if (bShouldSimulate)
        {
            PropertyTables.clear();
            SelectedEntities.clear();
            LastSelectedEntity = ECS::NullEntity;
            DetailsEntity = ECS::NullEntity;
            bDetailsDirty = true;
            bSimulatingWorld = true;

            // Leftover tags on ProxyWorld confuse the outliner rebuild on Simulate-exit.
            ECS::GetWorldRegistry(*World).ClearComponent<FSelectedInEditorComponent>();
            ECS::GetWorldRegistry(*World).ClearComponent<FLastSelectedTag>();

            FTransform TransformCopy = ECS::GetWorldRegistry(*World).Get<STransformComponent>(EditorEntity).GetWorldTransform();
            SCameraComponent CameraCopy = ECS::GetWorldRegistry(*World).Get<SCameraComponent>(EditorEntity);

            World->SetActive(false);
            ProxyWorld = World;
            ProxyEditorEntity = EditorEntity;

            // StartPIE returns null when the world cannot be duplicated, so bail before dereferencing it.
            CWorld* SimWorld = GWorldManager->StartPIE(ProxyWorld, EWorldType::Simulation, ENetMode::Standalone);
            if (SimWorld == nullptr)
            {
                LOG_WARN("Cannot Simulate '{0}': world has no package (save the world before simulating).", World->GetName().c_str());
                ImGuiX::Notifications::NotifyError("Cannot Simulate: save the world first.");
                World->SetActive(true);
                ProxyWorld = nullptr;
                ProxyEditorEntity = ECS::NullEntity;
                bSimulatingWorld = false;
                return;
            }
            RebindToWorld(SimWorld);

            if (ProxyEditorEntity != ECS::NullEntity && ECS::GetWorldRegistry(*ProxyWorld).IsValid(ProxyEditorEntity))
            {
                ProxyWorld->DestroyEntity(ProxyEditorEntity);
            }
            ProxyEditorEntity = ECS::NullEntity;
            EditorEntity = ECS::NullEntity;

            SetupWorldForTool();
            ASSERT(ECS::GetWorldRegistry(*World).IsValid(EditorEntity));

            WorldSettingsPropertyTable = MakeUnique<FPropertyTable>(&World->GetDefaultWorldSettings(), SDefaultWorldSettings::StaticStruct());

            ECS::GetWorldRegistry(*World).Get<STransformComponent>(EditorEntity).SetLocalTransform(TransformCopy);

            ECS::GetWorldRegistry(*World).Patch<SCameraComponent>(EditorEntity, [CameraCopy](SCameraComponent& Patch)
            {
                Patch = CameraCopy;
            });

            OutlinerListView.ClearTree();
            OutlinerListView.MarkTreeDirty();

            RebindRegistryObservers();
        }
        else
        {
            PropertyTables.clear();
            ResetSelectionState();
            DetailsEntity = ECS::NullEntity;
            bDetailsDirty = true;
            bSimulatingWorld = false;

            FTransform TransformCopy = ECS::GetWorldRegistry(*World).Get<STransformComponent>(EditorEntity).GetWorldTransform();
            SCameraComponent CameraCopy = ECS::GetWorldRegistry(*World).Get<SCameraComponent>(EditorEntity);

            if (EditorEntity != ECS::NullEntity && ECS::GetWorldRegistry(*World).IsValid(EditorEntity))
            {
                World->DestroyEntity(EditorEntity);
            }
            EditorEntity = ECS::NullEntity;
            ProxyEditorEntity = ECS::NullEntity;

            SetWorld(ProxyWorld);
            ProxyWorld->SetActive(true);
            ASSERT(ECS::GetWorldRegistry(*World).IsValid(EditorEntity));

            WorldSettingsPropertyTable = MakeUnique<FPropertyTable>(&World->GetDefaultWorldSettings(), SDefaultWorldSettings::StaticStruct());

            ECS::GetWorldRegistry(*World).Get<STransformComponent>(EditorEntity).SetLocalTransform(TransformCopy);

            ECS::GetWorldRegistry(*World).Patch<SCameraComponent>(EditorEntity, [CameraCopy](SCameraComponent& Patch)
            {
                Patch = CameraCopy;
            });

            ProxyWorld = nullptr;

            OutlinerListView.ClearTree();
            OutlinerListView.MarkTreeDirty();

            if (InputViewport)
            {
                FInputViewportRegistry::Get().SetActiveViewport(InputViewport.get());
                InputViewport->GetContext().SetInputMode(EInputMode::Game);
                FInputProcessor::Get().SetMouseMode(EMouseMode::Normal);
            }
        }
    }

    void FWorldEditorTool::ApplyInputFocus()
    {
        ImGuiIO& IO = ImGui::GetIO();
        const ImGuiConfigFlags Mask = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
        if (InputFocus == EInputFocus::Game)
        {
            IO.ConfigFlags |= Mask;
        }
        else
        {
            IO.ConfigFlags &= ~Mask;
        }
    }

    void FWorldEditorTool::SetInputFocus(EInputFocus NewFocus)
    {
        InputFocus = NewFocus;

        // One global state, so handing the cursor back reaches every PIE preview window at once.
        FInputViewportRegistry::Get().SetGameInputFocused(NewFocus == EInputFocus::Game);
    }

    bool FWorldEditorTool::OnEvent(FEvent& Event)
    {
        // Game focus hides the key from ImGui, and stopping tears down the world, so it defers to Update.
        if (bGamePreviewRunning && Event.IsA<FKeyPressedEvent>())
        {
            FKeyPressedEvent& Key = Event.As<FKeyPressedEvent>();
            if (Key.GetKeyCode() == EKey::Escape && !Key.IsRepeat())
            {
                bStopPlayRequested = true;
                return true;
            }
        }
        return FEditorTool::OnEvent(Event);
    }

    void FWorldEditorTool::HandleEntityEditorDragDrop(FTreeListView& Tree, ECS::FEntity DropItem)
    {
        // Inspects the typed payload rather than racing two AcceptDragDropPayload calls.
        const DragDrop::FPayload* Peek = DragDrop::PeekPayload();
        if (Peek == nullptr)
        {
            return;
        }

        if (Peek->Kind == DragDrop::EPayloadKind::Entity)
        {
            CWorld* OutWorld = nullptr;
            ECS::FEntity SourceEntity = ECS::NullEntity;
            if (DragDrop::AcceptEntity(&OutWorld, &SourceEntity) && OutWorld == World)
            {
                ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

                if (IsLockedPrefabChild(Registry, SourceEntity) || IsLockedPrefabChild(Registry, DropItem))
                {
                    ImGuiX::Notifications::NotifyError("Cannot reparent prefab-instance children. Edit the source prefab instead.");
                    return;
                }

                BeginRelationshipTransaction({ SourceEntity }, DropItem);
                RecordSceneFolderSnapshot();
                ECS::Utils::ReparentEntity(Registry, SourceEntity, DropItem);

                // An attached entity is shown under its parent, so its folder would be a hidden second home.
                if (SSceneFolderComponent* Folders = GetEditableSceneFolders())
                {
                    Folders->RemoveEntity(SourceEntity);
                }
                EntityFolderCache.erase(SourceEntity);

                EndTransaction("Reparent");

                ReparentEntityInOutliner(SourceEntity);
            }
            return;
        }

        AcceptContentBrowserPrefabPayload(DropItem, /*bAttachToTarget*/ true);
    }

    void FWorldEditorTool::HandleFolderDragDrop(uint32 FolderID)
    {
        const DragDrop::FPayload* Peek = DragDrop::PeekPayload();
        if (Peek == nullptr)
        {
            return;
        }

        if (Peek->Kind == DragDrop::EPayloadKind::Entity)
        {
            CWorld* OutWorld = nullptr;
            ECS::FEntity SourceEntity = ECS::NullEntity;
            if (DragDrop::AcceptEntity(&OutWorld, &SourceEntity) && OutWorld == World)
            {
                // Dragging one of several selected entities moves the whole selection.
                MoveEntitiesToFolder(GetComponentEditTargets(SourceEntity), FolderID);
            }
            return;
        }

        if (Peek->Kind == DragDrop::EPayloadKind::SceneFolder)
        {
            CWorld* OutWorld = nullptr;
            uint32 SourceFolder = 0;
            if (DragDrop::AcceptSceneFolder(&OutWorld, &SourceFolder) && OutWorld == World)
            {
                MoveFolderIntoFolder(SourceFolder, FolderID);
            }
            return;
        }

        PendingAssetDropFolder = FolderID;
        AcceptContentBrowserPrefabPayload(ECS::NullEntity, /*bAttachToTarget*/ false);
        PendingAssetDropFolder = 0;
    }

    void FWorldEditorTool::AcceptContentBrowserPrefabPayload(ECS::FEntity DropTarget, bool bAttachToTarget)
    {
        const DragDrop::FPayload* Peek = DragDrop::PeekPayload();
        if (Peek == nullptr || Peek->Kind != DragDrop::EPayloadKind::Asset)
        {
            return;
        }
        if (!DragDrop::IsDelivered())
        {
            return;
        }
        HandlePrefabContentDrop(FStringView(Peek->AssetPath.c_str(), Peek->AssetPath.size()), DropTarget, bAttachToTarget);
    }

    void FWorldEditorTool::HandlePrefabContentDrop(FStringView VirtualPath, ECS::FEntity DropTarget, bool bAttachToTarget)
    {
        // Dispatches every asset class via the editor drop registry. Spawn transform comes from the camera.
        const uint32 DropFolder = PendingAssetDropFolder;
        PendingAssetDropFolder = 0;

        BeginTransaction();
        ECS::FEntity Spawned = HandleContentBrowserAssetDrop(VirtualPath, DropTarget, bAttachToTarget);
        if (Spawned != ECS::NullEntity)
        {
            // Filed inside the same transaction so the drop is one undo step, not two.
            if (DropFolder != 0)
            {
                if (SSceneFolderComponent* Folders = GetEditableSceneFolders())
                {
                    Folders->AssignEntity(Spawned, DropFolder);
                    EntityFolderCache[Spawned] = DropFolder;
                }
            }

            EndTransaction("Drop Asset");
            SetSingleSelectedEntity(Spawned);
            OutlinerListView.MarkTreeDirty();
        }
        else
        {
            AbortTransaction();
            ImGuiX::Notifications::NotifyWarning("Could not add \"{0}\" to the world (see log for details).",
                FString(VirtualPath.data(), VirtualPath.size()).c_str());
        }
    }

    void FWorldEditorTool::DrawWorldSettings(bool bFocused)
    {
        WorldSettingsPropertyTable->DrawTree();
    }

    // Lists up to three stages, otherwise a count.
    static FString SystemStageSummary(const TVector<EUpdateStage>& Stages)
    {
        if (Stages.empty())
        {
            return FString();
        }
        if (Stages.size() > 3)
        {
            return FString(Format("{}", Stages.size()).c_str()) + " stages";
        }
        FString Out;
        for (size_t i = 0; i < Stages.size(); ++i)
        {
            if (i > 0)
            {
                Out += "  ";
            }
            Out += GUpdateStageNames[(uint8)Stages[i]];
        }
        return Out;
    }

    // Returns true when the row body is clicked, and sets OutTrash when the trash button is.
    static bool DrawSystemRow(const char* Icon, const ImVec4& Accent, const char* Name, const FString& Stages,
                              bool bDimmed, bool bShowTrash, const char* Tooltip, bool* OutTrash)
    {
        const ImVec4 RowBg       = EditorColors::RowBg();
        const ImVec4 RowBgHover  = EditorColors::RowBgHovered();
        const ImVec4 RowBgActive = EditorColors::RowBgActive();
        const ImVec4 TextPrimary = EditorColors::TextPrimary();
        const ImVec4 TextDim     = EditorColors::TextDim();
        const ImVec4 TextMuted   = EditorColors::TextMuted();
        const ImVec4 Danger      = EditorColors::Danger();

        const float  Scale  = ImGuiX::GetUIScale();
        const float  Avail  = ImGui::GetContentRegionAvail().x;
        const float  Height = 30.0f * Scale;
        const float  TrashW = bShowTrash ? 30.0f * Scale : 0.0f;
        const float  Round  = 4.0f * Scale;
        const ImVec2 P0     = ImGui::GetCursorScreenPos();
        const ImVec2 P1     = ImVec2(P0.x + Avail, P0.y + Height);

        ImGui::SetCursorScreenPos(P0);
        // Floored, since InvisibleButton asserts on an exactly-zero dimension at a dragged-flat panel.
        const float RowWidth = Avail - TrashW;
        const bool bClicked = ImGui::InvisibleButton("##row", ImVec2(RowWidth < 1.0f ? 1.0f : RowWidth, Height));
        const bool bHovered = ImGui::IsItemHovered();
        const bool bActive  = ImGui::IsItemActive();
        if (bHovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (Tooltip != nullptr)
            {
                ImGui::SetTooltip("%s", Tooltip);
            }
        }

        ImDrawList* DL = ImGui::GetWindowDrawList();
        DL->AddRectFilled(P0, P1, ImGui::ColorConvertFloat4ToU32(bActive ? RowBgActive : bHovered ? RowBgHover : RowBg), Round);
        DL->AddRectFilled(P0, ImVec2(P0.x + 3.0f * Scale, P1.y), ImGui::ColorConvertFloat4ToU32(Accent), Round);

        const float TextY = P0.y + (Height - ImGui::GetFontSize()) * 0.5f;

        ImGui::SetCursorScreenPos(ImVec2(P0.x + 13.0f * Scale, TextY));
        ImGui::PushStyleColor(ImGuiCol_Text, Accent);
        ImGui::TextUnformatted(Icon);
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(P0.x + 38.0f * Scale, TextY));
        ImGui::PushStyleColor(ImGuiCol_Text, bDimmed ? TextMuted : TextPrimary);
        ImGui::TextUnformatted(Name);
        ImGui::PopStyleColor();

        if (!Stages.empty())
        {
            const float StagesW = ImGui::CalcTextSize(Stages.c_str()).x;
            ImGui::SetCursorScreenPos(ImVec2(P1.x - TrashW - 12.0f * Scale - StagesW, TextY));
            ImGui::PushStyleColor(ImGuiCol_Text, TextDim);
            ImGui::TextUnformatted(Stages.c_str());
            ImGui::PopStyleColor();
        }

        if (bShowTrash)
        {
            const ImVec2 T0 = ImVec2(P1.x - TrashW, P0.y);
            ImGui::SetCursorScreenPos(T0);
            const bool bTrash      = ImGui::InvisibleButton("##trash", ImVec2(TrashW, Height));
            const bool bTrashHover = ImGui::IsItemHovered();
            if (bTrashHover)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                DL->AddRectFilled(T0, P1, ImGui::ColorConvertFloat4ToU32(ImVec4(Danger.x, Danger.y, Danger.z, 0.16f)), Round);
            }
            const float IconW = ImGui::CalcTextSize(LE_ICON_TRASH_CAN).x;
            ImGui::SetCursorScreenPos(ImVec2(T0.x + (TrashW - IconW) * 0.5f, TextY));
            ImGui::PushStyleColor(ImGuiCol_Text, bTrashHover ? Danger : TextDim);
            ImGui::TextUnformatted(LE_ICON_TRASH_CAN);
            ImGui::PopStyleColor();
            if (OutTrash != nullptr && bTrash)
            {
                *OutTrash = true;
            }
        }
        
        ImGui::SetCursorScreenPos(ImVec2(P0.x, P1.y + 3.0f * Scale));
        return bClicked;
    }

    void FWorldEditorTool::DrawSystemsPanel(bool bFocused)
    {
        if (World == nullptr)
        {
            return;
        }

        const ImVec4 TextDim   = EditorColors::TextDim();
        const ImVec4 TextMuted = EditorColors::TextMuted();
        const ImVec4 Section   = EditorColors::SectionHeader();
        const ImVec4 AccentOn  = EditorColors::Success();    // enabled native system
        const ImVec4 AccentOff = EditorColors::TextMuted();  // disabled native system

        TVector<CWorld::FSystemInfo> Systems;
        World->GetAllSystems(Systems);
        Algo::Sort(Systems, [](const CWorld::FSystemInfo& A, const CWorld::FSystemInfo& B)
        {
            return strcmp(A.Name.c_str(), B.Name.c_str()) < 0;
        });

        int32 EnabledCount = 0;
        for (const CWorld::FSystemInfo& Info : Systems)
        {
            EnabledCount += Info.bEnabled ? 1 : 0;
        }

        // Pinned search field with a leading magnifier.
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, TextDim);
        ImGui::TextUnformatted(LE_ICON_MAGNIFY);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        SystemsFilter.Draw("##SystemSearch", ImGui::GetContentRegionAvail().x);
        ImGui::Spacing();

        auto SectionHeader = [&](const char* Icon, const char* Label, const FString& Suffix, const char* Help)
        {
            ImGui::Spacing();
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::SmallBold);
            ImGui::PushStyleColor(ImGuiCol_Text, Section);
            ImGui::Text("%s  %s", Icon, Label);
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();
            if (!Suffix.empty())
            {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, TextMuted);
                ImGui::TextUnformatted(Suffix.c_str());
                ImGui::PopStyleColor();
            }
            if (Help != nullptr)
            {
                ImGui::SameLine();
                ImGuiX::HelpMarker(Help);
            }
            ImGui::Spacing();
        };

        auto EmptyState = [&](const char* Text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, TextMuted);
            ImGui::Indent(13.0f);
            ImGui::TextUnformatted(Text);
            ImGui::Unindent(13.0f);
            ImGui::PopStyleColor();
        };

        if (ImGui::BeginChild("##SystemsList", ImVec2(0, 0), false))
        {
            //~ Native systems ---------------------------------------------------------------------------
            SectionHeader(LE_ICON_CHIP, "NATIVE",
                FString(Format("{}", EnabledCount).c_str()) + " / " + Format("{}", Systems.size()).c_str() + " enabled",
                "Engine C++ systems for this world. Click a row to enable/disable it (a disabled system stops ticking "
                "here -- write your own to replace it). Changes apply next frame and save with the world.");

            bool bAnyNative = false;
            for (const CWorld::FSystemInfo& Info : Systems)
            {
                if (!ImGuiX::PassSearchFilter(SystemsFilter, Info.Name.c_str()))
                {
                    continue;
                }
                bAnyNative = true;

                ImGui::PushID(Info.Name.c_str());
                const bool bClicked = DrawSystemRow(LE_ICON_CHIP, Info.bEnabled ? AccentOn : AccentOff, Info.Name.c_str(),
                    SystemStageSummary(Info.Stages), !Info.bEnabled, false,
                    Info.bEnabled ? "Enabled -- click to disable" : "Disabled -- click to enable", nullptr);
                if (bClicked)
                {
                    World->SetSystemEnabled(Info.Name, !Info.bEnabled);
                    MarkSceneDirty();
                }
                ImGui::PopID();
            }
            if (!bAnyNative)
            {
                EmptyState("No systems match the filter.");
            }

            // A real zero-size item as the LAST op clears IsSetPos before EndChild checks bounds.
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }
        ImGui::EndChild();
    }

    void FWorldEditorTool::HandleOutlinerEmptyAreaDrop()
    {
        const DragDrop::FPayload* Peek = DragDrop::PeekPayload();

        // Empty space is the root, so an entity dropped here leaves both its folder and its parent.
        if (Peek != nullptr && Peek->Kind == DragDrop::EPayloadKind::Entity)
        {
            CWorld* OutWorld = nullptr;
            ECS::FEntity SourceEntity = ECS::NullEntity;
            if (DragDrop::AcceptEntity(&OutWorld, &SourceEntity) && OutWorld == World)
            {
                MoveEntitiesToFolder(GetComponentEditTargets(SourceEntity), 0);
            }
            return;
        }

        if (Peek != nullptr && Peek->Kind == DragDrop::EPayloadKind::SceneFolder)
        {
            CWorld* OutWorld = nullptr;
            uint32 SourceFolder = 0;
            if (DragDrop::AcceptSceneFolder(&OutWorld, &SourceFolder) && OutWorld == World)
            {
                MoveFolderIntoFolder(SourceFolder, 0);
            }
            return;
        }

        AcceptContentBrowserPrefabPayload(ECS::NullEntity, /*bAttachToTarget*/ false);
    }

    void FWorldEditorTool::DrawDetailsHeaderExtraButtons(ECS::FEntity Entity)
    {
        // Add-Tag button, sits alongside the shared Add-Component button (inside the green style scope).
        const float ActionDim = ImGui::GetFrameHeight();
        if (ImGui::Button(LE_ICON_TAG, ImVec2(ActionDim, ActionDim)))
        {
            PushAddTagModal(Entity);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Add Tag");
        }
    }

    void FWorldEditorTool::DrawDetailsExtraSections(ECS::FEntity Entity)
    {
        bool bHasTags = false;
        for (Lumina::ECS::FSparseSet* StoragePtr : ECS::GetWorldRegistry(*World).GetActiveStorages())
        {
            const Lumina::ECS::FComponentTypeID Name = StoragePtr->GetTypeInfo().TypeID;
            Lumina::ECS::FSparseSet& Storage = *StoragePtr;
            if (Name == ECS::GetComponentTypeID<STagComponent>() && Storage.Contains(Entity))
            {
                bHasTags = true;
                break;
            }
        }

        if (bHasTags)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(LE_ICON_PUZZLE " Tags");
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            DrawTagList(Entity);
        }
    }

    void FWorldEditorTool::DrawEntityActionButtons(ECS::FEntity Entity)
    {
        constexpr float ButtonHeight = 32.0f;
        const float AvailWidth = ImGui::GetContentRegionAvail().x;
        const float ButtonWidth = (AvailWidth - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.65f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.5f, 0.25f, 1.0f));

        if (ImGui::Button(LE_ICON_PLUS " Add Component", ImVec2(ButtonWidth, ButtonHeight)))
        {
            PushAddComponentModal(Entity);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Add a new component to this entity");
        }

        ImGui::SameLine();

        if (ImGui::Button(LE_ICON_TAG " Add Tag", ImVec2(ButtonWidth, ButtonHeight)))
        {
            PushAddTagModal(Entity);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Add a runtime tag to this entity to use with runtime views.");
        }

        ImGui::PopStyleColor(3);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));

        if (ImGui::Button(LE_ICON_TRASH_CAN " Destroy", ImVec2(AvailWidth, ButtonHeight)))
        {
            if (Dialogs::Confirmation("Confirm Deletion", "Are you sure you want to delete entity \"{0}\"?\n""\nThis action cannot be undone.", (uint32)Entity))
            {
                EntityDestroyRequests.push(Entity);
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Permanently delete this entity");
        }

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }

    void FWorldEditorTool::DrawTagList(ECS::FEntity Entity)
    {

        TFixedVector<FName, 4> Tags;
        for (Lumina::ECS::FSparseSet* StoragePtr : ECS::GetWorldRegistry(*World).GetActiveStorages())
        {
            const Lumina::ECS::FComponentTypeID Name = StoragePtr->GetTypeInfo().TypeID;
            Lumina::ECS::FSparseSet& Storage = *StoragePtr;
            if (Name == ECS::GetComponentTypeID<STagComponent>())
            {
                if (Storage.Contains(Entity))
                {
                    STagComponent* ComponentPtr = static_cast<STagComponent*>(Storage.GetRaw(Entity));
                    Tags.push_back(ComponentPtr->Tag);
                }
            }
        }
        
        if (Tags.empty())
        {
            return;
        }
        
        ImGui::PushID("TagList");
        
        ImVec2 CursorPos = ImGui::GetCursorScreenPos();
        ImVec2 HeaderSize = ImVec2(ImGui::GetContentRegionAvail().x, 32.0f);
        
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        DrawList->AddRectFilled(CursorPos, ImVec2(CursorPos.x + HeaderSize.x, CursorPos.y + HeaderSize.y), IM_COL32(25, 25, 30, 255), 6.0f);
        
        DrawList->AddRect(CursorPos, ImVec2(CursorPos.x + HeaderSize.x, CursorPos.y + HeaderSize.y), IM_COL32(45, 45, 52, 255), 6.0f, 0, 1.0f);
        
        ImVec2 IconPos = CursorPos;
        IconPos.x += 12.0f;
        IconPos.y += (HeaderSize.y - ImGui::GetTextLineHeight()) * 0.5f;
        DrawList->AddText(IconPos, IM_COL32(150, 170, 200, 255), LE_ICON_TAG);
        
        ImVec2 TitlePos = IconPos;
        TitlePos.x += 24.0f;
        DrawList->AddText(TitlePos, IM_COL32(220, 220, 230, 255), "Tags");
        
        char CountBuf[16];
        snprintf(CountBuf, sizeof(CountBuf), "%zu", Tags.size());
        ImVec2 CountPos = TitlePos;
        CountPos.x += ImGui::CalcTextSize("Tags").x + 8.0f;
        CountPos.y -= 1.0f;
        
        ImVec2 CountBadgeSize = ImGui::CalcTextSize(CountBuf);
        CountBadgeSize.x += 10.0f;
        CountBadgeSize.y += 2.0f;
        
        DrawList->AddRectFilled(CountPos, 
            ImVec2(CountPos.x + CountBadgeSize.x, CountPos.y + CountBadgeSize.y),
            IM_COL32(60, 80, 120, 180), 3.0f);
        DrawList->AddText(ImVec2(CountPos.x + 5.0f, CountPos.y + 1.0f), 
            IM_COL32(180, 200, 240, 255), CountBuf);
        
        ImGui::SetCursorScreenPos(ImVec2(CursorPos.x, CursorPos.y + HeaderSize.y + 4.0f));
        
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 6.0f));
        
        float AvailWidth = ImGui::GetContentRegionAvail().x;
        float CurrentX = 0.0f;
        
        FName TagToRemove;
        
        for (const FName& Tag : Tags)
        {
            ImGui::PushID(Tag.c_str());
            
            const char* TagStr = Tag.c_str();
            ImVec2 TagSize = ImGui::CalcTextSize(TagStr);
            float ChipWidth = TagSize.x + 52.0f;
            
            if (CurrentX + ChipWidth > AvailWidth && CurrentX > 0.0f)
            {
                CurrentX = 0.0f;
            }
            else if (CurrentX > 0.0f)
            {
                ImGui::SameLine();
            }
            
            ImVec2 ChipPos = ImGui::GetCursorScreenPos();
            ImVec2 ChipSize = ImVec2(ChipWidth, TagSize.y + 10.0f);
            
            bool bHovered = ImGui::IsMouseHoveringRect(ChipPos, 
                ImVec2(ChipPos.x + ChipSize.x, ChipPos.y + ChipSize.y));
            
            ImU32 ChipBg = bHovered ? IM_COL32(55, 65, 85, 255) : IM_COL32(45, 55, 75, 255);
            ImU32 ChipBorder = bHovered ? IM_COL32(80, 100, 140, 255) : IM_COL32(65, 80, 115, 255);
            
            DrawList->AddRectFilled(ChipPos, ImVec2(ChipPos.x + ChipSize.x, ChipPos.y + ChipSize.y), ChipBg, 12.0f);
            DrawList->AddRect(ChipPos, ImVec2(ChipPos.x + ChipSize.x, ChipPos.y + ChipSize.y), ChipBorder, 12.0f, 0, 1.0f);
            
            DrawList->AddText(ImVec2(ChipPos.x + 10.0f, ChipPos.y + 5.0f), IM_COL32(130, 160, 210, 255), LE_ICON_TAG);
            
            DrawList->AddText(ImVec2(ChipPos.x + 28.0f, ChipPos.y + 5.0f), IM_COL32(200, 210, 230, 255), TagStr);
            
            ImVec2 ClosePos = ImVec2(ChipPos.x + ChipSize.x - 20.0f, ChipPos.y + 5.0f);
            bool bCloseHovered = ImGui::IsMouseHoveringRect(ImVec2(ClosePos.x - 4.0f, ClosePos.y - 4.0f), ImVec2(ClosePos.x + 12.0f, ClosePos.y + 12.0f));
            
            ImU32 CloseColor = bCloseHovered ? IM_COL32(240, 100, 100, 255) : IM_COL32(150, 150, 160, 255);
            DrawList->AddText(ClosePos, CloseColor, LE_ICON_CLOSE);
            
            ImGui::InvisibleButton("##chip", ChipSize);
            
            if (bCloseHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                TagToRemove = Tag;
            }
            
            CurrentX += ChipWidth + ImGui::GetStyle().ItemSpacing.x;
            
            ImGui::PopID();
        }
        
        ImGui::PopStyleVar(3);
        
        if (!TagToRemove.IsNone())
        {
            ECS::GetWorldRegistry(*World).NamedStorage<STagComponent>(TagToRemove).RemoveEntity(Entity);
        }
        
        ImGui::Spacing();
        ImGui::PopID();
    }

    bool FWorldEditorTool::IsUnsavedDocument()
    {
        return World && World->GetPackage() && World->GetPackage()->IsDirty();
    }

    void FWorldEditorTool::GroupSelectedEntities()
    {
        if (World == nullptr || World->IsSimulating())
        {
            return;
        }

        TFixedVector<ECS::FEntity, 64> Targets;
        Targets.reserve(SelectedEntities.size());
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        for (ECS::FEntity Entity : SelectedEntities)
        {
            if (!Registry.IsValid(Entity) || IsLockedPrefabChild(Registry, Entity))
            {
                continue;
            }
            Targets.push_back(Entity);
        }

        if (Targets.size() < 2)
        {
            return;
        }

        FVector3 Median(0.0f);
        for (ECS::FEntity Entity : Targets)
        {
            Median += Registry.Get<STransformComponent>(Entity).GetWorldLocation();
        }
        Median /= static_cast<float>(Targets.size());

        BeginTransaction();

        FTransform GroupTransform;
        GroupTransform.SetLocation(Median);
        ECS::FEntity Group = World->ConstructEntity("Group", GroupTransform);
        if (Group == ECS::NullEntity)
        {
            AbortTransaction();
            return;
        }

        for (ECS::FEntity Entity : Targets)
        {
            ECS::Utils::ReparentEntity(Registry, Entity, Group);
            ReparentEntityInOutliner(Entity);
        }

        SetSingleSelectedEntity(Group);
        EndTransaction("Group Selected");
    }

    namespace
    {
        // Writes a world-space transform onto an entity, converting through the parent when it has one.
        void ApplyWorldTransform(ECS::FRegistry& Registry, ECS::FEntity Entity, const FTransform& DesiredWorld)
        {
            STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

            const FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Entity);
            if (Rel != nullptr && Rel->Parent != ECS::NullEntity && Registry.IsValid(Rel->Parent))
            {
                const FMatrix4 ParentWorld = Registry.Get<STransformComponent>(Rel->Parent).GetWorldMatrix();
                const FMatrix4 NewLocal    = Math::Inverse(ParentWorld) * DesiredWorld.GetMatrix();

                FVector3 LocalTranslation, LocalScale, Skew;
                FQuat    LocalRotation;
                FVector4 Perspective;
                Math::Decompose(NewLocal, LocalScale, LocalRotation, LocalTranslation, Skew, Perspective);

                Transform.SetLocalLocation(LocalTranslation);
                Transform.SetLocalRotation(LocalRotation);
                Transform.SetLocalScale(LocalScale);
                return;
            }

            Transform.SetLocalLocation(DesiredWorld.GetLocation());
            Transform.SetLocalRotation(DesiredWorld.GetRotation());
        }
    }

    void FWorldEditorTool::OpenTransformDialog()
    {
        if (World == nullptr || World->IsSimulating() || ToolContext == nullptr)
        {
            return;
        }

        ToolContext->PushModal("Transform", ImVec2(420.0f, 300.0f), [this]() -> bool
        {
            ImGui::TextUnformatted("Applied to every selected entity, in its own local space.");
            ImGui::Separator();

            ImGui::DragFloat3("Translate", &TransformDialog.Translate.x, 0.05f);
            ImGui::DragFloat3("Rotate", &TransformDialog.RotateDegrees.x, 0.5f, -360.0f, 360.0f, "%.2f deg");
            ImGui::DragFloat3("Scale", &TransformDialog.Scale.x, 0.01f);

            ImGui::Separator();

            int32 OrderIndex = TransformDialog.Order == ETransformDialogOrder::Pre ? 0 : 1;
            if (ImGui::Combo("Order", &OrderIndex, "Pre\0Post\0"))
            {
                TransformDialog.Order = OrderIndex == 0 ? ETransformDialogOrder::Pre : ETransformDialogOrder::Post;
            }
            ImGuiX::WrappedTooltip("{}", TransformDialog.Order == ETransformDialogOrder::Pre
                ? "Pre multiplies the delta before the entity's transform, so it acts in parent space."
                : "Post multiplies it after, so it acts along the entity's own axes.");

            ImGui::Separator();

            if (ImGui::Button("Reset"))
            {
                TransformDialog = FTransformDialogState{};
            }

            ImGui::SameLine();
            const bool bApply = ImGui::Button("Apply");
            ImGui::SameLine();
            const bool bClose = ImGui::Button("Close");

            if (bApply)
            {
                ApplyTransformDialog();
            }

            // Apply keeps the dialog up, so a value can be nudged and re-applied without reopening.
            return bClose;
        });
    }

    void FWorldEditorTool::ApplyTransformDialog()
    {
        if (World == nullptr || World->IsSimulating())
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        TVector<ECS::FEntity> Targets;
        Targets.reserve(GetSelectedEntities().size());
        for (ECS::FEntity Entity : GetSelectedEntities())
        {
            if (Registry.IsValid(Entity) && Registry.HasAll<STransformComponent>(Entity)
                && !IsLockedPrefabChild(Registry, Entity))
            {
                Targets.push_back(Entity);
            }
        }

        if (Targets.empty())
        {
            return;
        }

        FTransform Delta;
        Delta.SetLocation(TransformDialog.Translate);
        Delta.SetRotationFromEuler(TransformDialog.RotateDegrees);
        Delta.SetScale(TransformDialog.Scale);
        const FMatrix4 DeltaMatrix = Delta.GetMatrix();

        BeginTransformTransaction(Targets);

        for (ECS::FEntity Entity : Targets)
        {
            STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
            FTransform LocalTransform;
            LocalTransform.SetLocation(Transform.GetLocalLocation());
            LocalTransform.SetRotation(Transform.GetLocalRotation());
            LocalTransform.SetScale(Transform.GetLocalScale());
            const FMatrix4 Local = LocalTransform.GetMatrix();

            const FMatrix4 Result = TransformDialog.Order == ETransformDialogOrder::Pre
                                  ? DeltaMatrix * Local
                                  : Local * DeltaMatrix;

            FVector3 Translation, Scale, Skew;
            FQuat    Rotation;
            FVector4 Perspective;
            Math::Decompose(Result, Scale, Rotation, Translation, Skew, Perspective);

            Transform.SetLocalLocation(Translation);
            Transform.SetLocalRotation(Rotation);
            Transform.SetLocalScale(Scale);
        }

        EndTransaction("Transform Dialog");
    }

    void FWorldEditorTool::AlignSelectionToView(bool bRotationOnly)
    {
        if (World == nullptr || World->IsSimulating() || EditorEntity == ECS::NullEntity)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (!Registry.IsValid(EditorEntity))
        {
            return;
        }

        const FTransform ViewTransform = Registry.Get<STransformComponent>(EditorEntity).GetWorldTransform();

        TVector<ECS::FEntity> Targets;
        Targets.reserve(GetSelectedEntities().size());
        for (ECS::FEntity Entity : GetSelectedEntities())
        {
            if (Registry.IsValid(Entity) && Registry.HasAll<STransformComponent>(Entity)
                && !IsLockedPrefabChild(Registry, Entity))
            {
                Targets.push_back(Entity);
            }
        }

        if (Targets.empty())
        {
            return;
        }

        BeginTransformTransaction(Targets);

        for (ECS::FEntity Entity : Targets)
        {
            FTransform Desired = Registry.Get<STransformComponent>(Entity).GetWorldTransform();
            Desired.SetRotation(ViewTransform.GetRotation());
            if (!bRotationOnly)
            {
                Desired.SetLocation(ViewTransform.GetLocation());
            }

            ApplyWorldTransform(Registry, Entity, Desired);
        }

        EndTransaction(bRotationOnly ? "Align Rotation with View" : "Align Transform with View");
    }

    void FWorldEditorTool::AlignViewToSelection()
    {
        if (World == nullptr || !HasEditorCameraControl())
        {
            return;
        }

        const ECS::FEntity Entity = GetLastSelectedEntity();
        if (!World->IsValidEntity(Entity))
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (!Registry.HasAll<STransformComponent>(Entity))
        {
            return;
        }

        const FTransform Target = Registry.Get<STransformComponent>(Entity).GetWorldTransform();

        // Orbit derives the camera from yaw/pitch/distance, so it would overwrite an aligned placement.
        SetCameraMode(EEditorCameraMode::Free);

        FEditorCameraState& State = GetCameraState();
        State.FocusFreePosition = Target.GetLocation();
        State.FocusFreeRotation = Target.GetRotation();
        State.bFocusInterp      = true;
    }

    void FWorldEditorTool::DropSelectionToFloor()
    {
        if (World == nullptr || World->IsSimulating())
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        TFixedVector<ECS::FEntity, 64> Targets;
        Targets.reserve(SelectedEntities.size());
        for (ECS::FEntity Entity : SelectedEntities)
        {
            if (Registry.IsValid(Entity) && !IsLockedPrefabChild(Registry, Entity))
            {
                Targets.push_back(Entity);
            }
        }

        if (Targets.empty())
        {
            return;
        }

        // Build exclusion set (dropped entities + descendants) so a group doesn't land on its own child mesh.
        THashSet<ECS::FEntity> Exclude;
        for (ECS::FEntity Entity : Targets)
        {
            Exclude.insert(Entity);
            ECS::Utils::ForEachDescendant(Registry, Entity, [&](ECS::FEntity Desc)
            {
                Exclude.insert(Desc);
            });
        }

        // Snapshot world-space AABBs once so per-entity raycast is a flat vector walk.
        struct FCandidate { FAABB Box; };
        TVector<FCandidate> Candidates;
        Registry.View<STransformComponent, SStaticMeshComponent>().ForEach(
            [&](ECS::FEntity Entity, STransformComponent& Transform, SStaticMeshComponent& Mesh)
        {
            if (Exclude.find(Entity) != Exclude.end())
            {
                return;
            }
            Candidates.push_back({ Mesh.GetAABB().ToWorld(Transform.GetWorldMatrix()) });
        });

        TVector<ECS::FEntity, 0> TransactionTargets(Targets.begin(), Targets.end());
        BeginTransformTransaction(TransactionTargets);

        bool bAnyMoved = false;
        const FVector3 Down(0.0f, -1.0f, 0.0f);

        for (ECS::FEntity Entity : Targets)
        {
            STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
            const FVector3 WorldLocation = Transform.GetWorldLocation();
            const FVector3 Origin = WorldLocation + FVector3(0.0f, 0.5f, 0.0f);

            float BestT = FLT_MAX;
            for (const FCandidate& C : Candidates)
            {
                float T;
                if (RayVsAABB(Origin, Down, C.Box, T) && T < BestT)
                {
                    BestT = T;
                }
            }

            // Fallback to Y=0 plane so the action always does something predictable.
            float NewY;
            if (BestT < FLT_MAX)
            {
                NewY = (Origin + Down * BestT).y;
            }
            else
            {
                NewY = 0.0f;
            }

            FTransform NewWorld = Transform.GetWorldTransform();
            FVector3 NewLoc = NewWorld.GetLocation();
            NewLoc.y = NewY;
            NewWorld.SetLocation(NewLoc);

            FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Entity);
            if (Rel != nullptr && Rel->Parent != ECS::NullEntity && Registry.IsValid(Rel->Parent))
            {
                FMatrix4 ParentWorld = Registry.Get<STransformComponent>(Rel->Parent).GetWorldMatrix();
                FMatrix4 NewLocalMat = Math::Inverse(ParentWorld) * NewWorld.GetMatrix();

                FVector3 LT, LS, LSkew; FQuat LR; FVector4 LP;
                Math::Decompose(NewLocalMat, LS, LR, LT, LSkew, LP);

                Transform.SetLocalLocation(LT);
                Transform.SetLocalRotation(LR);
                Transform.SetLocalScale(LS);
            }
            else
            {
                Transform.SetLocalLocation(NewWorld.GetLocation());
            }

            bAnyMoved = true;
        }

        if (bAnyMoved)
        {
            EndTransaction("Drop to Floor");
        }
        else
        {
            AbortTransaction();
        }
    }

    void FWorldEditorTool::FrameAllEntities()
    {
        if (World == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (!Registry.IsValid(EditorEntity))
        {
            return;
        }

        FVector3 Min(FLT_MAX);
        FVector3 Max(-FLT_MAX);
        bool bAny = false;

        auto View = Registry.View<STransformComponent>();
        for (ECS::FEntity Entity : View)
        {
            if (Entity == EditorEntity)
            {
                continue;
            }

            const FVector3 Loc = Registry.Get<STransformComponent>(Entity).GetWorldLocation();
            Min = Math::Min(Min, Loc);
            Max = Math::Max(Max, Loc);
            bAny = true;
        }

        if (!bAny)
        {
            return;
        }

        const FVector3 Center = (Min + Max) * 0.5f;
        const float Radius = Math::Max(Math::Length(Max - Min) * 0.5f, 1.0f);

        const SCameraComponent& Camera = Registry.Get<SCameraComponent>(EditorEntity);
        const float HalfFov = Math::Radians(Camera.GetFOV() * 0.5f);
        const float Distance = (Radius / Math::Tan(Math::Max(HalfFov, Math::Radians(1.0f)))) * 1.5f;

        STransformComponent& EditorTransform = Registry.Get<STransformComponent>(EditorEntity);
        const FVector3 Forward = EditorTransform.GetForward();
        const FVector3 NewPos  = Center - Forward * Distance;
        EditorTransform.SetLocation(NewPos);
        EditorTransform.SetRotation(Math::FindLookAtRotation(Center, NewPos));
    }

    void FWorldEditorTool::CopyTransformFromLastSelected()
    {
        if (World == nullptr)
        {
            return;
        }

        const ECS::FEntity Entity = GetLastSelectedEntity();
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (!Registry.IsValid(Entity))
        {
            return;
        }

        CopiedTransform = Registry.Get<STransformComponent>(Entity).GetWorldTransform();
        bHasCopiedTransform = true;
    }

    void FWorldEditorTool::PasteTransformToSelection()
    {
        if (World == nullptr || World->IsSimulating() || !bHasCopiedTransform)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        TFixedVector<ECS::FEntity, 64> Targets;
        Targets.reserve(SelectedEntities.size());
        for (ECS::FEntity Entity : SelectedEntities)
        {
            if (Registry.IsValid(Entity) && !IsLockedPrefabChild(Registry, Entity))
            {
                Targets.push_back(Entity);
            }
        }

        if (Targets.empty())
        {
            return;
        }

        TVector<ECS::FEntity, 0> TransactionTargets(Targets.begin(), Targets.end());
        BeginTransformTransaction(TransactionTargets);

        for (ECS::FEntity Entity : Targets)
        {
            STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

            FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Entity);
            if (Rel != nullptr && Rel->Parent != ECS::NullEntity && Registry.IsValid(Rel->Parent))
            {
                FMatrix4 ParentWorld = Registry.Get<STransformComponent>(Rel->Parent).GetWorldMatrix();
                FMatrix4 NewLocalMat = Math::Inverse(ParentWorld) * CopiedTransform.GetMatrix();

                FVector3 LT, LS, LSkew; FQuat LR; FVector4 LP;
                Math::Decompose(NewLocalMat, LS, LR, LT, LSkew, LP);

                Transform.SetLocalLocation(LT);
                Transform.SetLocalRotation(LR);
                Transform.SetLocalScale(LS);
            }
            else
            {
                Transform.SetLocalTransform(CopiedTransform);
            }
        }

        EndTransaction("Paste Transform");
    }

    void FWorldEditorTool::SaveCameraBookmark(int32 Slot)
    {
        if (Slot < 0 || Slot >= NumCameraBookmarks || World == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (!Registry.IsValid(EditorEntity))
        {
            return;
        }

        CameraBookmarks[Slot] = Registry.Get<STransformComponent>(EditorEntity).GetWorldTransform();
        bCameraBookmarkSet[Slot] = true;
    }

    void FWorldEditorTool::RecallCameraBookmark(int32 Slot)
    {
        if (Slot < 0 || Slot >= NumCameraBookmarks || !bCameraBookmarkSet[Slot] || World == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (!Registry.IsValid(EditorEntity))
        {
            return;
        }

        // Restore to free-cam; orbit re-derives from the saved pose on next input.
        if (CameraState.Mode != EEditorCameraMode::Free)
        {
            SetCameraMode(EEditorCameraMode::Free);
        }

        STransformComponent& Transform = Registry.Get<STransformComponent>(EditorEntity);
        Transform.SetLocalLocation(CameraBookmarks[Slot].GetLocation());
        Transform.SetLocalRotation(CameraBookmarks[Slot].GetRotation());
        Transform.SetLocalScale(CameraBookmarks[Slot].GetScale());
    }

    void FWorldEditorTool::DrawCursorWorldPositionOverlay(ImVec2 ViewportOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera)
    {
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
        {
            return;
        }

        const ImVec2 MousePos = ImGui::GetMousePos();
        const float LocalX = MousePos.x - ViewportOrigin.x;
        const float LocalY = MousePos.y - ViewportOrigin.y;
        if (LocalX < 0.0f || LocalY < 0.0f || LocalX >= ViewportSize.x || LocalY >= ViewportSize.y)
        {
            return;
        }

        // Unproject through camera ViewProj; flip Y because camera bakes Vulkan +Y-down NDC.
        FMatrix4 Proj = Camera.GetProjectionMatrix();
        Proj[1][1] *= -1.0f;
        const FMatrix4 InvVP = Math::Inverse(Proj * Camera.GetViewMatrix());

        const float NdcX = (LocalX / ViewportSize.x) * 2.0f - 1.0f;
        const float NdcY = 1.0f - (LocalY / ViewportSize.y) * 2.0f;
        FVector4 Far = InvVP * FVector4(NdcX, NdcY, 1.0f, 1.0f);
        if (Math::Abs(Far.w) < 1e-6f)
        {
            return;
        }
        const FVector3 FarWorld = FVector3(Far) / Far.w;
        const FVector3 Origin   = Camera.GetPosition();
        const FVector3 Dir      = Math::Normalize(FarWorld - Origin);

        // Intersect Y=0 plane; skip nearly-parallel rays or up-pointing rays from above.
        if (Math::Abs(Dir.y) < 1e-4f)
        {
            return;
        }
        const float T = -Origin.y / Dir.y;
        if (T <= 0.0f)
        {
            return;
        }
        const FVector3 Hit = Origin + Dir * T;

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const ImVec2 TextPos(ViewportOrigin.x + 12.0f, ViewportOrigin.y + ViewportSize.y - 24.0f);
        char Buf[128];
        snprintf(Buf, sizeof(Buf), "Cursor: %.2f, %.2f, %.2f", Hit.x, Hit.y, Hit.z);
        DrawList->AddRectFilled(ImVec2(TextPos.x - 6.0f, TextPos.y - 4.0f),
                                ImVec2(TextPos.x + 220.0f, TextPos.y + 18.0f),
                                IM_COL32(0, 0, 0, 140), 4.0f);
        DrawList->AddText(TextPos, IM_COL32(220, 220, 220, 230), Buf);
    }

    void FWorldEditorTool::DrawEntityDebugOverlay(ImVec2 ViewportOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera)
    {
        if (!bDrawEntityDebugInfo || World == nullptr)
        {
            return;
        }

        FMatrix4 Proj = Camera.GetProjectionMatrix();
        Proj[1][1] *= -1.0f;
        const FMatrix4 ViewProj = Proj * Camera.GetViewMatrix();
        FFrustum Frustum = Camera.GetViewVolume().GetFrustum();
        const FVector3 CameraPos = Camera.GetPosition();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        auto View = Registry.View<STransformComponent>(ECS::TExclude<FEditorComponent>{});

        struct FCandidate
        {
            ECS::FEntity Entity;
            ImVec2       Screen;
            float        DepthSq;
        };

        TVector<FCandidate> Candidates;
        Candidates.reserve(View.Num());

        for (ECS::FEntity Entity : View)
        {
            const STransformComponent& Transform = View.Get<STransformComponent>(Entity);
            const FVector3 WorldPos = Transform.GetWorldLocation();
            if (!Frustum.IsInside(WorldPos))
            {
                continue;
            }

            ImVec2 Screen;
            if (!ProjectPointToScreen(WorldPos, ViewProj, ViewportSize, Screen))
            {
                continue;
            }

            const FVector3 Delta = WorldPos - CameraPos;
            Candidates.push_back({ Entity, Screen, Math::Dot(Delta, Delta) });
        }

        // Front-to-back so closer labels claim space first.
        Algo::Sort(Candidates, [](const FCandidate& A, const FCandidate& B)
        {
            return A.DepthSq < B.DepthSq;
        });

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const float LineHeight = ImGui::GetTextLineHeight();
        const float Padding = 4.0f;
        const ImU32 BgColor = IM_COL32(0, 0, 0, 160);
        const ImU32 TextColor = IM_COL32(230, 230, 230, 235);

        TVector<ImRect> PlacedRects;
        PlacedRects.reserve(Candidates.size());

        for (const FCandidate& C : Candidates)
        {
            const SNameComponent* NameComp = Registry.TryGet<SNameComponent>(C.Entity);
            const STransformComponent& Transform = Registry.Get<STransformComponent>(C.Entity);
            const FVector3 P = Transform.GetWorldLocation();

            char Line0[96];
            char Line1[96];
            const char* Name = (NameComp && !NameComp->Name.IsNone()) ? NameComp->Name.c_str() : "Entity";
            snprintf(Line0, sizeof(Line0), "%s (id=%u)", Name, (uint32)(C.Entity).Value);
            snprintf(Line1, sizeof(Line1), "%.2f, %.2f, %.2f", P.x, P.y, P.z);

            const ImVec2 Size0 = ImGui::CalcTextSize(Line0);
            const ImVec2 Size1 = ImGui::CalcTextSize(Line1);
            const float BoxW = Math::Max(Size0.x, Size1.x) + Padding * 2.0f;
            const float BoxH = LineHeight * 2.0f + Padding * 2.0f;

            // Anchor above the entity, then nudge down on collision until clear or we give up.
            ImVec2 Anchor(ViewportOrigin.x + C.Screen.x - BoxW * 0.5f,
                          ViewportOrigin.y + C.Screen.y - BoxH - 6.0f);

            // Clamp to viewport horizontally so labels don't drift offscreen.
            const float MinX = ViewportOrigin.x + 2.0f;
            const float MaxX = ViewportOrigin.x + ViewportSize.x - BoxW - 2.0f;
            Anchor.x = Math::Clamp(Anchor.x, MinX, MaxX);

            ImRect Rect(Anchor, ImVec2(Anchor.x + BoxW, Anchor.y + BoxH));

            bool bPlaced = false;
            for (int32 Attempt = 0; Attempt < 16; ++Attempt)
            {
                bool bOverlap = false;
                for (const ImRect& Other : PlacedRects)
                {
                    if (Rect.Overlaps(Other))
                    {
                        bOverlap = true;
                        Rect.Min.y = Other.Max.y + 1.0f;
                        Rect.Max.y = Rect.Min.y + BoxH;
                        break;
                    }
                }
                if (!bOverlap)
                {
                    bPlaced = true;
                    break;
                }
            }

            if (!bPlaced)
            {
                continue;
            }

            // Drop labels that got pushed off the bottom of the viewport.
            if (Rect.Max.y > ViewportOrigin.y + ViewportSize.y - 2.0f)
            {
                continue;
            }

            PlacedRects.push_back(Rect);

            DrawList->AddRectFilled(Rect.Min, Rect.Max, BgColor, 3.0f);
            DrawList->AddText(ImVec2(Rect.Min.x + Padding, Rect.Min.y + Padding), TextColor, Line0);
            DrawList->AddText(ImVec2(Rect.Min.x + Padding, Rect.Min.y + Padding + LineHeight), TextColor, Line1);
        }
    }

    void FWorldEditorTool::DrawOffscreenSelectionIndicators(ImVec2 ViewportOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera)
    {
        if (World == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        auto SelView = Registry.View<FSelectedInEditorComponent, STransformComponent>();
        if (SelView.Num() == 0)
        {
            return;
        }

        FMatrix4 Proj = Camera.GetProjectionMatrix();
        Proj[1][1] *= -1.0f;
        const FMatrix4 ViewProj = Proj * Camera.GetViewMatrix();

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const float EdgePadding = 32.0f;
        const ImVec2 Center(ViewportOrigin.x + ViewportSize.x * 0.5f,
                            ViewportOrigin.y + ViewportSize.y * 0.5f);
        const ImVec2 RectMin(ViewportOrigin.x + EdgePadding,
                             ViewportOrigin.y + EdgePadding);
        const ImVec2 RectMax(ViewportOrigin.x + ViewportSize.x - EdgePadding,
                             ViewportOrigin.y + ViewportSize.y - EdgePadding);

        const ImU32 FillColor    = IM_COL32(255, 195, 60, 235);
        const ImU32 OutlineColor = IM_COL32(20, 20, 20, 220);

        for (ECS::FEntity Entity : SelView)
        {
            const STransformComponent& Transform = SelView.Get<STransformComponent>(Entity);
            const FVector3 WorldPos = Transform.GetWorldLocation();

            FVector4 Clip = ViewProj * FVector4(WorldPos, 1.0f);

            // Reflect points behind the camera through origin so the indicator points back toward the entity.
            const bool bBehind = Clip.w <= 0.0f;
            if (bBehind)
            {
                Clip.x = -Clip.x;
                Clip.y = -Clip.y;
                Clip.w = -Clip.w;
            }
            const float SafeW = Math::Max(Clip.w, 1e-4f);
            float NdcX = Clip.x / SafeW;
            float NdcY = Clip.y / SafeW;

            // Force NDC outside [-1,1] for behind-camera entities so we still emit an indicator.
            if (bBehind)
            {
                const float Mag = Math::Max(Math::Abs(NdcX), Math::Abs(NdcY));
                if (Mag > 1e-4f)
                {
                    const float Scale = 1.5f / Mag;
                    NdcX *= Scale;
                    NdcY *= Scale;
                }
                else
                {
                    NdcX = 0.0f;
                    NdcY = -1.5f;
                }
            }

            const float ScreenX = (NdcX * 0.5f + 0.5f) * ViewportSize.x + ViewportOrigin.x;
            const float ScreenY = (1.0f - (NdcY * 0.5f + 0.5f)) * ViewportSize.y + ViewportOrigin.y;

            if (!bBehind &&
                ScreenX >= RectMin.x && ScreenX <= RectMax.x &&
                ScreenY >= RectMin.y && ScreenY <= RectMax.y)
            {
                continue;
            }

            ImVec2 Dir(ScreenX - Center.x, ScreenY - Center.y);
            const float Len = sqrtf(Dir.x * Dir.x + Dir.y * Dir.y);
            if (Len < 1e-3f)
            {
                continue;
            }
            Dir.x /= Len;
            Dir.y /= Len;

            // Clip ray from center to the inset rect.
            const float TX = (Dir.x > 0.0f) ? (RectMax.x - Center.x) / Dir.x
                            : (Dir.x < 0.0f) ? (RectMin.x - Center.x) / Dir.x
                            : FLT_MAX;
            const float TY = (Dir.y > 0.0f) ? (RectMax.y - Center.y) / Dir.y
                            : (Dir.y < 0.0f) ? (RectMin.y - Center.y) / Dir.y
                            : FLT_MAX;
            const float T = Math::Min(TX, TY);
            if (T <= 0.0f)
            {
                continue;
            }

            const ImVec2 Tip(Center.x + Dir.x * T, Center.y + Dir.y * T);

            const float ArrowLen  = 16.0f;
            const float ArrowHalf = 9.0f;
            const ImVec2 Perp(-Dir.y, Dir.x);
            const ImVec2 BaseCenter(Tip.x - Dir.x * ArrowLen, Tip.y - Dir.y * ArrowLen);
            const ImVec2 BaseL(BaseCenter.x + Perp.x * ArrowHalf, BaseCenter.y + Perp.y * ArrowHalf);
            const ImVec2 BaseR(BaseCenter.x - Perp.x * ArrowHalf, BaseCenter.y - Perp.y * ArrowHalf);

            DrawList->AddTriangleFilled(Tip, BaseL, BaseR, FillColor);
            DrawList->AddTriangle(Tip, BaseL, BaseR, OutlineColor, 1.5f);
            DrawList->AddCircleFilled(BaseCenter, 3.0f, FillColor);
            DrawList->AddCircle(BaseCenter, 3.0f, OutlineColor, 0, 1.5f);
        }
    }
}
