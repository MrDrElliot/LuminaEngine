#include <string>
#include "Tools/Import/ImportPaths.h"
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
#include "Input/InputViewport.h"
#include "UI/RmlUiBridge.h"
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
#include "ModelingEditMode.h"
#include "SequencerEditMode.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "UI/Tools/EditorEntityUtils.h"
#include "Scene/DefaultScene.h"
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
#include "UI/Widgets/ComponentPicker.h"
#include "UI/Widgets/EntityScriptPicker.h"
#include "World/Entity/Systems/SystemAccess.h"


namespace Lumina
{
    static constexpr const char* WorldSettingsName = "World Settings";

    // The console has no context pointer, so the command reaches the world editor through this.
    static FWorldEditorTool* GActiveWorldEditor = nullptr;

    static FAutoConsoleCommand GToggleGameViewCommand("Editor.ToggleGameView",
        "Hide or show editor overlays such as the grid and gizmos, as the G key does over the viewport.", []
        {
            if (GActiveWorldEditor != nullptr)
            {
                GActiveWorldEditor->ToggleGameViewMode();
            }
        });
    static FAutoConsoleCommand GShadowRangeDemoCommand("Editor.ShadowRangeDemo",
        "Open a transient world built to test sun shadows from a few meters out to several kilometers.", []
        {
            if (GActiveWorldEditor != nullptr)
            {
                GActiveWorldEditor->OpenShadowRangeDemo();
            }
        });
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
        if (Registry.HasAll<SSelectionRoot>(Entity))
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

        const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();
        for (ECS::FEntity Parent = Hierarchy.GetParent(Entity); Parent != ECS::NullEntity; Parent = Hierarchy.GetParent(Parent))
        {
            if (Registry.HasAll<SSelectionRoot>(Parent))
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
        }

        return Entity;
    }

    // The editor world has no physics scene, so mesh AABBs are projected in software.

    // Project world AABB to screen rect (y-down, viewport pixels). Returns false if the box is behind near plane.
    static bool ProjectAABBToScreenRect(const SAABB& WorldAABB, const FMatrix4& ViewProj,
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

    // Distance straight down from Origin to the box's top, zero when Origin is already inside it.
    static bool CastDownToAABB(const FVector3& Origin, const SAABB& Box, float& OutT)
    {
        const bool bOverXZ = Origin.x >= Box.Min.x && Origin.x <= Box.Max.x && Origin.z >= Box.Min.z && Origin.z <= Box.Max.z;
        if (!bOverXZ || Origin.y < Box.Min.y)
        {
            return false;
        }
        OutT = Math::Max(0.0f, Origin.y - Box.Max.y);
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
        const TVector<FMeshlet>& Meshlets = MeshletData.Meshlets;

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
                    Visit(GetMeshletVertexPosition(MeshletData, M, v, false));
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
        const SAABB WorldAABB = Mesh.GetAABB().ToWorld(MeshWorldMatrix);
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
        GActiveWorldEditor = this;
        CreateToolWindow(SceneGraphName, [&] (bool bFocused)
        {
            DrawOutliner(bFocused);
        });
        
        CreateToolWindow("Details", [&] (bool bFocused)
        {
            DrawDetailsPanel(bFocused);
        });

        CreateToolWindow(WorldSettingsName, [&](bool bFocused)
        {
            DrawWorldSettings(bFocused);
        });

        CreateToolWindow(SystemsName, [&](bool bFocused)
        {
            DrawSystemsPanel(bFocused);
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
                DragDrop::SetSceneFolderPayload(World.Get(), Data.FolderID);
                return;
            }

            DragDrop::SetEntityPayload(World.Get(), Data.Entity);
        };

        OutlinerContext.ItemContextMenuFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            if (Data.FolderID != 0)
            {
                DrawFolderContextMenu(Data.FolderID);
                return;
            }

            ECS::FRegistry& Registry = World->GetEntityRegistry();
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
            DrawAddComponentMenu(Data.Entity);
            DrawAddCollisionMenuItem(Data.Entity);
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
            if (!bLocked && Registry.GetHierarchy().GetParent(Data.Entity) != ECS::NullEntity)
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
                            const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(Data.Entity);
                            BeginRelationshipTransaction({ Data.Entity }, Parent);
                            World->GetAnimation()->AttachEntityToSocket(Data.Entity, Parent, Socket);
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

            if (!bLocked && Registry.GetHierarchy().GetChildCount(Data.Entity) != 0)
            {
                if (ImGui::MenuItem(LE_ICON_CALL_SPLIT " Detach Children"))
                {
                    // Snapshot child IDs before mutating relationships, then move each in the tree.
                    TFixedVector<ECS::FEntity, 20> Children;
                    Registry.GetHierarchy().ForEachChild(Data.Entity, [&](ECS::FEntity Child) { Children.push_back(Child); });
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
                const bool bIsSelectionRoot = Registry.HasAll<SSelectionRoot>(Data.Entity);
                const char* RootLabel = bIsSelectionRoot
                    ? LE_ICON_TARGET " Unmark Selection Root"
                    : LE_ICON_TARGET " Mark as Selection Root";
                if (ImGui::MenuItem(RootLabel))
                {
                    if (bIsSelectionRoot)
                    {
                        Registry.Remove<SSelectionRoot>(Data.Entity);
                    }
                    else
                    {
                        Registry.Emplace<SSelectionRoot>(Data.Entity);
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

            // Disabled is saved with the level and changes what the game runs, so it is an edit like any other.
            BeginComponentTransaction({ Data.Entity }, SDisabledTag::StaticStruct());
            if (State.bDisabled)
            {
                World->GetEntityRegistry().Emplace<SDisabledTag>(Data.Entity);
            }
            else
            {
                World->GetEntityRegistry().Remove<SDisabledTag>(Data.Entity);
            }
            EndTransaction(State.bDisabled ? "Disable Entity" : "Enable Entity");
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
                World->GetEntityRegistry().EmplaceOrReplace<SScriptDisabledTag>(Data.Entity);
            }
            else
            {
                World->GetEntityRegistry().Remove<SScriptDisabledTag>(Data.Entity);
            }

            if (World->GetPackage() != nullptr)
            {
                World->GetPackage()->MarkDirty();
            }
        };

        OutlinerContext.HoveredFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FEntityListViewItemData& Data = Tree.Get<FEntityListViewItemData>(Item);
            ECS::FRegistry& Registry = World->GetEntityRegistry();
            if (!Registry.IsValid(Data.Entity))
            {
                return;
            }

            // Tooltip building (component scan) is deferred from tree-build to first hover.
            BuildEntityTooltip(Data.Entity, Tree.Get<FTreeNodeDisplay>(Item));

            EditorEntityUtils::DrawEntityBounds(World.Get(), Data.Entity, FColor::White, 3.0f);
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

            EditorEntityUtils::RenameOutlinerRow(Tree.Get<FTreeNodeDisplay>(Item), NewName);

            SNameComponent& NameComponent = World->GetEntityRegistry().Get<SNameComponent>(Data.Entity);
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
                    if (Ops->Has(World->GetEntityRegistry(), Entity) == 0)
                    {
                        bPasses = false;
                    }
                }
            }
            
            return bPasses;
        };


        RebindRegistryObservers();

        WorldTraveledHandle = FCoreDelegates::OnWorldTraveled.AddMember(this, &FWorldEditorTool::OnWorldTraveled);
        GameQuitHandle = FCoreDelegates::Get().OnGameQuitRequested.AddMember(this, &FWorldEditorTool::OnGameQuitRequested);
    }

    void FWorldEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        if (GActiveWorldEditor == this)
        {
            GActiveWorldEditor = nullptr;
        }
        FCoreDelegates::OnWorldTraveled.Remove(WorldTraveledHandle);
        WorldTraveledHandle = FDelegateHandle{};
        FCoreDelegates::Get().OnGameQuitRequested.Remove(GameQuitHandle);
        GameQuitHandle = FDelegateHandle{};

        if (bSimulatingWorld)
        {
            SetWorldNewSimulate(false);
        }

        if (bGamePreviewRunning)
        {
            // Closing the tool mid-play ends PIE too, so subscribers must not be left holding the world.
            FCoreEditorDelegates::OnPIEEnd.Broadcast(World.Get());
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

        if (World != nullptr && !bGameViewMode)
        {
            EntityScripts::DebugDraw(World->GetEntityRegistry());
        }

        // Scripts' debug windows join the editor's own while playing, as a game build's overlay shows them.
        if (World != nullptr && HasSimulatingWorld())
        {
            EntityScripts::DebugGui(World->GetEntityRegistry());
        }

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
                if (!World->GetEntityRegistry().IsValid(Entity))
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
                if (World->GetEntityRegistry().IsValid(Selected))
                {
                    World->GetEntityRegistry().EmplaceOrReplace<FNeedsTransformUpdate>(Selected);
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
                if (World->GetEntityRegistry().IsValid(Selected))
                {
                    CurrentSelection.push_back(Selected);
                }
            }

            for (ECS::FEntity SelectedEntity : CurrentSelection)
            {
                const bool bLocked = IsLockedPrefabChild(World->GetEntityRegistry(), SelectedEntity);

                if (bCopyPressed)
                {
                    AddEntityToCopies(SelectedEntity);
                }

                if (bDeletePressed && !bLocked)
                {
                    EntityDestroyRequests.push(SelectedEntity);
                }
            }

            if (bDuplicatePressed)
            {
                DuplicateAndSelect(TSpan<const ECS::FEntity>(CurrentSelection.data(), CurrentSelection.size()));
            }
        }
        
        constexpr size_t kMaxIndividualSelectionBoxes = 256;

        if (!bGameViewMode && SelectedEntities.size() <= kMaxIndividualSelectionBoxes)
        {
            for (ECS::FEntity Entity : SelectedEntities)
            {
                if (!World->GetEntityRegistry().IsValid(Entity))
                {
                    continue;
                }

                // Every selectable type gets the same box, with a unit-box fallback resolved by the shared helper.
                EditorEntityUtils::DrawEntitySelectionBox(World.Get(), Entity, FColor::Green, 0.2f, 5.0f);
            }
        }

        const bool bPastePressed = bSelectionEditActive
            && ImGui::IsKeyDown(ImGuiKey_LeftCtrl)
            && ImGui::IsKeyPressed(ImGuiKey_V, false);

        if (bPastePressed)
        {
            // Snapshot sources first; CopyEntity adds rows the view would otherwise re-paste.
            TFixedVector<ECS::FEntity, 64> CopySources;
            World->GetEntityRegistry().View<FCopiedTag>().ForEach([&](ECS::FEntity Entity)
            {
                if (!IsLockedPrefabChild(World->GetEntityRegistry(), Entity))
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

    void FWorldEditorTool::OpenShadowRangeDemo()
    {
        if (HasSimulatingWorld())
        {
            LOG_WARN("Stop play before opening the shadow range demo.");
            return;
        }

        CWorld* DemoWorld = NewObject<CWorld>(nullptr, "Shadow Range Demo", FGuid::New(), OF_Transient);
        SetWorld(DemoWorld);
        DefaultScene::PopulateShadowRangeDemo(DemoWorld);

        const DefaultScene::FCameraPose Pose = DefaultScene::GetShadowRangeCameraPose();
        const ECS::FEntity Camera = DemoWorld->GetCamera()->GetActiveCameraEntity();
        STransformComponent* Transform = Camera != ECS::NullEntity && DemoWorld->IsValidEntity(Camera)
            ? DemoWorld->TryGetComponent<STransformComponent>(Camera) : nullptr;
        if (Transform != nullptr)
        {
            Transform->SetLocation(Pose.Location);
            Transform->SetRotation(Math::FindLookAtRotation(Pose.Target, Pose.Location));
        }
    }

    void FWorldEditorTool::ToggleGameViewMode()
    {
        if (!bGameViewMode)
        {
            bSavedWorldGridEnabled = bWorldGridEnabled;
            bSavedShowComponentVisualizers = bShowComponentVisualizers;
            bWorldGridEnabled = false;
            bShowComponentVisualizers = false;
            bGameViewMode = true;
        }
        else
        {
            bWorldGridEnabled = bSavedWorldGridEnabled;
            bShowComponentVisualizers = bSavedShowComponentVisualizers;
            bGameViewMode = false;
        }

        if (World != nullptr && World->GetRenderer() != nullptr)
        {
            World->GetRenderSettings().bHideEditorOverlays = bGameViewMode;
        }
    }

    void FWorldEditorTool::RegisterEditorActions()
    {
        // Hotkeys are suppressed while right-mouse flying so W/E/R/Q act as fly keys, not mode switches.
        auto Hovered      = [this]() { return bViewportHovered && !CameraState.bWasLooking; };

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

        // FEditorUI routes Ctrl+S to the focused tool, so a live callback here would save twice.
        RegisterAction({"Save World", "File", "Save the current world",
            FInputChord{ImGuiKey_S, true}, nullptr});

        // Advisory entries, inline-handled shortcuts registered so the shortcuts window surfaces them.
        RegisterAction({"Copy Entities", "Selection", "Copy the selection to the entity clipboard",
            FInputChord{ImGuiKey_C, true}, nullptr});
        
        RegisterAction({"Duplicate Entities", "Selection", "Duplicate the selection in place",
            FInputChord{ImGuiKey_D, true}, nullptr});

        RegisterAction({"Drag a Copy", "Selection", "Alt-drag the transform gizmo to drag a duplicate of the selection",
            FInputChord{}, nullptr});

        RegisterAction({"Camera Follows Drag", "Selection", "Hold Shift while moving the selection with the gizmo to bring the camera along",
            FInputChord{}, nullptr});
        
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
        EditorModes.push_back(MakeUnique<FModelingEditMode>());
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
            Active->OnEnter(World.Get());
        }
    }

    void FWorldEditorTool::UpdateSimulationGrab(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize, bool bInViewportHovered)
    {
        (void)ViewportOrigin;
        (void)ViewportSize;

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
            GrabbedTarget = {};
            return;
        }

        const bool bGrabHeld = ImGui::GetIO().KeyShift && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (GrabbedTarget.IsSet() && !bGrabHeld)
        {
            GrabbedTarget = {};
        }

        FVector3 RayOrigin, RayDirection;
        if (!BuildViewportRay(ImGui::GetMousePos(), RayOrigin, RayDirection))
        {
            return;
        }

        if (!GrabbedTarget.IsSet())
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

            Physics::FPhysicsBodyState BodyState;
            if (!Scene->TryGetTargetState(Hit->Target, BodyState) || BodyState.Mass <= 0.0f)
            {
                return;
            }
            GrabbedTarget = Hit->Target;
            GrabDistance = Math::Length(Hit->Location - RayOrigin);
            GrabLocalOffset = Math::Inverse(BodyState.Rotation)
                            * (Hit->Location - BodyState.Position);
            return;
        }

        Physics::FPhysicsBodyState BodyState;
        if (!Scene->TryGetTargetState(GrabbedTarget, BodyState) || BodyState.Mass <= 0.0f)
        {
            GrabbedTarget = {};
            return;
        }
        const FVector3 Target = RayOrigin + RayDirection * GrabDistance;
        const FVector3 AttachPoint = BodyState.Position
                                   + BodyState.Rotation * GrabLocalOffset;

        // Capped so a fast cursor asks for a drag rather than a launch, which stops the body arriving instantly.
        FVector3 DesiredVelocity = (Target - AttachPoint) * ApproachGain;

        const float DesiredSpeed = Math::Length(DesiredVelocity);
        if (DesiredSpeed > MaxGrabSpeed)
        {
            DesiredVelocity *= MaxGrabSpeed / DesiredSpeed;
        }

        // GetVelocityAtPoint includes spin, so a body rotating under the grab is corrected by the same term.
        FVector3 Acceleration = (DesiredVelocity - (BodyState.LinearVelocity + Math::Cross(BodyState.AngularVelocity, AttachPoint - BodyState.CenterOfMass)))
                              * VelocityGain;

        const float AccelerationMagnitude = Math::Length(Acceleration);
        if (AccelerationMagnitude > MaxGrabAcceleration)
        {
            Acceleration *= MaxGrabAcceleration / AccelerationMagnitude;
        }

        // A body reporting zero mass is static or kinematic, so the grab lets go rather than pulling on nothing.
        const float BodyMass = BodyState.Mass;
        if (BodyMass <= 0.0f)
        {
            GrabbedTarget = {};
            return;
        }

        Scene->AddForceAtTarget(GrabbedTarget, Acceleration * BodyMass, AttachPoint);

        World->GetDebugInterface()->DrawLine(AttachPoint, Target, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 2.5f, false);
        World->GetDebugInterface()->DrawSphere(Target, 0.035f, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 10, 2.0f, false);
    }

    IWorldEditorMode* FWorldEditorTool::GetActiveMode() const
    {
        if (EditorModes.empty()) return nullptr;
        const int32 Idx = Math::Clamp(ActiveModeIndex, 0, (int32)EditorModes.size() - 1);
        return EditorModes[Idx].get();
    }

    bool FWorldEditorTool::CanDuplicateEntity(ECS::FEntity Entity) const
    {
        return !IsLockedPrefabChild(World->GetEntityRegistry(), Entity);
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
            Old->OnExit(World.Get());
        }
        ActiveModeIndex = NewIndex;
        if (IWorldEditorMode* New = EditorModes[ActiveModeIndex].get())
        {
            New->OnEnter(World.Get());
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
        ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Down, 0.55f, &dockRightBottom, &dockRightTop);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(),    dockLeft);
        ImGui::DockBuilderDockWindow(GetToolWindowName(SceneGraphName).c_str(),        dockRightTop);
        ImGui::DockBuilderDockWindow(GetToolWindowName("Details").c_str(),             dockRightBottom);
        ImGui::DockBuilderDockWindow(GetToolWindowName(WorldSettingsName).c_str(),     dockRightBottom);
        ImGui::DockBuilderDockWindow(GetToolWindowName(SystemsName).c_str(),           dockRightBottom);
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
        if (!HasEditorCameraControl() || bGameViewMode)
        {
            return;
        }
        
        SCameraComponent& CameraComponent = World->GetEntityRegistry().Get<SCameraComponent>(EditorEntity);

        FMatrix4 ViewMatrix = CameraComponent.GetViewMatrix();
        FMatrix4 ProjectionMatrix = CameraComponent.GetProjectionMatrix();
        // Camera projection bakes Vulkan +Y-down NDC; ImGuizmo expects GL convention.
        ProjectionMatrix[1][1] *= -1.0f;

        const ImVec2 ViewportOrigin = ViewportScreenMin;

        ImGuizmo::SetDrawlist(ImGui::GetCurrentWindow()->DrawList);
        ImGuizmo::SetRect(ViewportOrigin.x, ViewportOrigin.y, ViewportSize.x, ViewportSize.y);

        const bool bDropHovering = DrawViewportDropTarget();

        if (IWorldEditorMode* ActiveMode = GetActiveMode())
        {
            ActiveMode->Tick(World.Get(), CameraComponent, bViewportHovered, ViewportOrigin, ViewportSize);
            ActiveMode->DrawOverlay(World.Get(), ViewportOrigin, ViewportSize, CameraComponent);
        }

        UpdateSimulationGrab(ViewportOrigin, ViewportSize, bViewportHovered);

        // Shift and click is also the marquee chord, so a poke at a ragdoll would select everything crossed.
        const bool bModeOwnsInput = (GetActiveMode() && GetActiveMode()->ConsumesViewportInput())
                                 || IsSimulationGrabActive();

        // TickEditorCamera runs in Update before this draw, so the flag is current-frame.
        const bool bCameraOwnsInput = ShouldSuppressViewportClickInput();

        const bool bAltGrabsGizmo = HandleAltDragCopy(bModeOwnsInput);

        auto SelectionView = World->GetEntityRegistry().View<FSelectedInEditorComponent, STransformComponent>();

        const ECS::FEntity PivotEntityForGizmo = GetLastSelectedEntity();
        const bool bGizmoTargetValid = SelectionView.Num() && World->GetEntityRegistry().IsValid(PivotEntityForGizmo);

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
                STransformComponent& PivotTransformComponent = World->GetEntityRegistry().Get<STransformComponent>(PivotEntity);

                // Padded AABB so the gizmo stays visible when the pivot is just outside the frustum but handles aren't.
                const FVector3 PivotWorld = PivotTransformComponent.GetWorldLocation();
                const SAABB PivotBounds(PivotWorld - FVector3(1.0f), PivotWorld + FVector3(1.0f));
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
                        const SStaticMeshComponent* PivotMC = World->GetEntityRegistry().TryGet<SStaticMeshComponent>(PivotEntity);
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
                    const bool bGizmoInert = bCameraOwnsInput && !bImGuizmoUsedOnce && !bAltGrabsGizmo;
                    if (bGizmoInert)
                    {
                        ImGuizmo::Enable(false);
                    }

                    // The camera's ride so far, which the follow below put into the cursor ray and the drag must not count again.
                    ImGuizmo::SetTranslationOffset(&GizmoFollowOffset.x);

                    FMatrix4 GizmoDeltaMatrix(1.0f);
                    ImGuizmo::Manipulate(Math::ValuePtr(ViewMatrix), Math::ValuePtr(ProjectionMatrix),
                        GuizmoOp, GuizmoMode, Math::ValuePtr(EntityMatrix), Math::ValuePtr(GizmoDeltaMatrix), SnapValues);

                    if (bGizmoInert)
                    {
                        ImGuizmo::Enable(true);
                    }
                    GetCameraState().bGizmoHovered = ImGuizmo::IsOver();

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
                            GizmoFollowOffset = FVector3(0.0f);
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
                                ECS::FRegistry& Registry = World->GetEntityRegistry();
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

                                if (const ECS::FEntity Parent = World->GetEntityRegistry().GetHierarchy().GetParent(Entity); Parent != ECS::NullEntity)
                                {
                                    STransformComponent& ParentTransform = World->GetEntityRegistry().Get<STransformComponent>(Parent);
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
                            World->GetEntityRegistry().EmplaceOrReplace<FNeedsTransformUpdate>(Selected);
                        }

                        // Shift keeps the camera with the moved selection, so a long drag never leaves the view.
                        if (GuizmoOp == ImGuizmo::TRANSLATE && ImGui::GetIO().KeyShift)
                        {
                            const FVector3 PivotDelta = GuizmoMode == ImGuizmo::LOCAL
                                ? FVector3(EntityMatrix[3]) - FVector3(PreManipulateMatrix[3])
                                : DeltaTranslation;
                            MoveEditorCameraBy(PivotDelta);
                            GizmoFollowOffset += PivotDelta;
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
                                DL->AddRect(ImVec2(P.x - 7, P.y - 7), ImVec2(P.x + 7, P.y + 7), C, 0.0f, 2.0f, 0);
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
        if (!bModeOwnsInput && !bCameraOwnsInput && !bCameraPreviewMouseOver && !AreVisualizersCapturingInput() && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && !RmlUi::WorldUIWantsMouse(World.Get()))
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
                    Hit = ResolveSelectionRootForViewportPick(World->GetEntityRegistry(), Hit);
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
                        EntityHandle = ResolveSelectionRootForViewportPick(World->GetEntityRegistry(), EntityHandle);

                        // On a picker miss keep existing selection so the menu still has something to act on.
                        if (EntityHandle != ECS::NullEntity && !IsEntitySelected(EntityHandle))
                        {
                            SetSingleSelectedEntity(EntityHandle);
                            RevealEntityInOutliner(EntityHandle);
                        }

                        const ECS::FEntity MenuTarget = (EntityHandle != ECS::NullEntity)
                            ? EntityHandle
                            : GetLastSelectedEntity();

                        if (World->GetEntityRegistry().IsValid(MenuTarget))
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
                    DrawList->AddRect(ScreenStart, ScreenEnd, IM_COL32(100, 150, 255, 255), 0.0f, 2.0f, 0);
                }
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && SelectionBox.bActive)
                {
                    ImVec2 Start = SelectionBox.Start;
                    ImVec2 End = SelectionBox.Current;
                    
                    if (!bLeftDragging)
                    {
                        ECS::FEntity EntityHandle = World->GetRenderer()->GetEntityAtPixel(TexX, TexY);
                        EntityHandle = ResolveSelectionRootForViewportPick(World->GetEntityRegistry(), EntityHandle);

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

                        ECS::FRegistry& Registry = World->GetEntityRegistry();

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
                                SAABB WorldAABB = Mesh->GetAABB().ToWorld(Transform.GetWorldMatrix());
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
        else if (!bDropHovering)
        {
            // Not a pick target this frame, so tell the renderer to skip the picker readback.
            World->GetRenderer()->SetPickerCursor(0, 0, false);
        }

        if (ImGui::BeginPopup("EntityContextMenu"))
        {
            const ECS::FEntity LastSelected = GetLastSelectedEntity();

            if (World->GetEntityRegistry().IsValid(LastSelected))
            {
                ECS::FRegistry& Registry = World->GetEntityRegistry();
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
                DrawAddComponentMenu(LastSelected);
                DrawAddCollisionMenuItem(LastSelected);
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

                if (!bLastSelectedLocked && Registry.GetHierarchy().GetParent(LastSelected) != ECS::NullEntity)
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

                if (!bLastSelectedLocked && Registry.GetHierarchy().GetChildCount(LastSelected) != 0)
                {
                    if (ImGui::MenuItem("Detach Children"))
                    {
                        TVector<ECS::FEntity> Children;
                        Registry.GetHierarchy().ForEachChild(LastSelected, [&](ECS::FEntity Child) { Children.push_back(Child); });
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
                    const bool bIsSelectionRoot = Registry.HasAll<SSelectionRoot>(LastSelected);
                    if (ImGui::MenuItem(bIsSelectionRoot ? "Unmark Selection Root" : "Mark as Selection Root"))
                    {
                        if (bIsSelectionRoot)
                        {
                            Registry.Remove<SSelectionRoot>(LastSelected);
                        }
                        else
                        {
                            Registry.Emplace<SSelectionRoot>(LastSelected);
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
            ActiveMode->DrawToolbar(World.Get(), ButtonSize);
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

    void FWorldEditorTool::DrawAddComponentMenu(ECS::FEntity Entity)
    {
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        if (!Registry.IsValid(Entity))
        {
            return;
        }

        ComponentAttachPicker.SearchHint = "Search components...";
        ComponentAttachPicker.ItemNoun   = "components";
        ComponentAttachPicker.TrailingCategory = EntityScriptPicker::NativeCategory;
        if (!ImGui::BeginMenu(LE_ICON_PUZZLE " Add Component"))
        {
            return;
        }

        // A multi-selection adds to every selected entity, so a component counts as present only once all of them have it.
        const TVector<ECS::FEntity> Targets = GetComponentEditTargets(Entity);
        auto IsOnEveryTarget = [&Registry, &Targets](CStruct* Struct)
        {
            return !Targets.empty() && Algo::AllOf(Targets, [&](ECS::FEntity Target)
            {
                return ECS::Utils::HasComponent(Registry, Target, Struct);
            });
        };

        if (const FPickerItem* Picked = ComponentAttachPicker.DrawContents([&](TVector<FPickerItem>& Out) { ComponentPicker::GatherItems(Out, IsOnEveryTarget); }))
        {
            if (CStruct* Component = ComponentPicker::FindComponent(Picked->Key))
            {
                ApplyAddComponentToTargets(Targets, Component);
                if (Entity == DetailsEntity)
                {
                    bDetailsDirty = true;
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }

    void FWorldEditorTool::DrawScriptAttachMenuItems(ECS::FEntity Entity)
    {
        ECS::FRegistry& Registry = World->GetEntityRegistry();

        if (!Registry.IsValid(Entity))
        {
            return;
        }

        const bool bHasScript = Registry.HasAll<SEntityScriptComponent>(Entity);

        // Always offered, since it either assigns a script or swaps the one already on the entity.
        ScriptAttachPicker.SearchHint = "Search scripts...";
        ScriptAttachPicker.ItemNoun   = "scripts";
        ScriptAttachPicker.TrailingCategory = EntityScriptPicker::NativeCategory;
        if (ImGui::BeginMenu(bHasScript ? LE_ICON_LANGUAGE_CSHARP " Change Script" : LE_ICON_LANGUAGE_CSHARP " Attach Script"))
        {
            if (const FPickerItem* Picked = ScriptAttachPicker.DrawContents(&EntityScriptPicker::GatherItems))
            {
                AttachScriptToEntity(Entity, Picked->Key);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
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
        ECS::FRegistry& Registry = World->GetEntityRegistry();
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
        ECS::FRegistry& Registry = World->GetEntityRegistry();
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

        if (!World->GetPackage()->IsDirty())
        {
            return;
        }

        if (!ToolContext->ConfirmWritable({ FString(World->GetPackage()->GetPackagePath().c_str()) }, [this] { OnSave(); }))
        {
            return;
        }

        // The world editor saves its live CWorld in place, and the thumbnail comes from the viewport.
        if (ShouldGenerateThumbnailOnSave())
        {
            GenerateThumbnail(World->GetPackage());
        }

        FCoreEditorDelegates::OnAssetPreSave.Broadcast(World.Get());

        if (CPackage::SavePackage(World->GetPackage(), World->GetPackage()->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetSaved(World.Get());
            FCoreEditorDelegates::OnAssetSaved.Broadcast(World.Get());
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
            ImGuiX::InputText("##Folder", DirBuffer);

            ImGui::Spacing();
            ImGui::TextUnformatted("Name");
            ImGui::SetNextItemWidth(-1.0f);
            const bool bEnter = ImGuiX::InputText("##Name", NameBuffer, ImGuiInputTextFlags_EnterReturnsTrue);

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
            // Free of loaded packages and in-flight imports as well as files, since CreatePackage refuses all three.
            Path = Import::PathReservations::FindFree(Path);
            if (Path.empty())
            {
                ErrorMessage = "No free name in that folder.";
                return false;
            }
            CPackage::AddPackageExt(Path);

            CPackage* NewPackage = CPackage::CreatePackage(Path);
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
                FAssetRegistry::Get().AssetCreated(World.Get());
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
                Registry.GetHierarchy().ForEachDescendant(Entity, [&](ECS::FEntity)
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

    TVector<ECS::FEntity> FWorldEditorTool::GatherPrefabSelectionRoots() const
    {
        ECS::FRegistry& Registry = World->GetEntityRegistry();

        // Drops invalid handles and prefab-instance children whose hierarchy is locked.
        THashSet<ECS::FEntity> Filtered;
        for (ECS::FEntity Entity : SelectedEntities)
        {
            if (Registry.IsValid(Entity) && !IsLockedPrefabChild(Registry, Entity))
            {
                Filtered.insert(Entity);
            }
        }

        // If any ancestor is also selected, the descent happens through the parent instead.
        TVector<ECS::FEntity> Roots;
        Roots.reserve(Filtered.size());
        for (ECS::FEntity Entity : Filtered)
        {
            const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();
            bool bAncestorInSet = false;
            for (ECS::FEntity Walk = Hierarchy.GetParent(Entity); Walk != ECS::NullEntity; Walk = Hierarchy.GetParent(Walk))
            {
                if (Filtered.find(Walk) != Filtered.end())
                {
                    bAncestorInSet = true;
                    break;
                }
            }
            if (!bAncestorInSet)
            {
                Roots.push_back(Entity);
            }
        }

        return Roots;
    }

    void FWorldEditorTool::PushCreatePrefabFromSelectionModal()
    {
        if (World == nullptr || World->IsSimulating())
        {
            ImGuiX::Notifications::NotifyWarning("Cannot create a prefab while simulating.");
            return;
        }

        TVector<ECS::FEntity> Roots = GatherPrefabSelectionRoots();
        if (Roots.empty())
        {
            ImGuiX::Notifications::NotifyWarning("Select an entity in the world before creating a prefab.");
            return;
        }

        ECS::FRegistry& Registry = World->GetEntityRegistry();
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
            ImGuiX::InputText("##Folder", DirBuffer);

            ImGui::Spacing();
            ImGui::TextUnformatted("Name");
            ImGui::SetNextItemWidth(-1.0f);
            const bool bEnter = ImGuiX::InputText("##Name", NameBuffer, ImGuiInputTextFlags_EnterReturnsTrue);

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

            if (CPrefab* Prefab = CreatePrefabFromRoots(Req.Roots, Req.Pivot, FStringView(DirBuffer.c_str()), FStringView(NameBuffer.c_str()), ErrorMessage))
            {
                ImGuiX::Notifications::NotifySuccess("Created prefab: \"{0}\"", Prefab->GetPackage()->GetPackagePath());
                return true;
            }
            return false;
        });
    }

    CPrefab* FWorldEditorTool::CreatePrefabFromRoots(const TVector<ECS::FEntity>& Roots, const FVector3& Pivot, FStringView Folder, FStringView Name, FFixedString& OutError)
    {
        if (Name.empty()) { OutError = "Name cannot be empty."; return nullptr; }
        if (Folder.empty()) { OutError = "Folder cannot be empty."; return nullptr; }

        ECS::FRegistry& WorkingRegistry = World->GetEntityRegistry();
        for (ECS::FEntity Entity : Roots)
        {
            if (!WorkingRegistry.IsValid(Entity))
            {
                OutError = "Selection changed; cannot capture missing entities.";
                return nullptr;
            }
        }

        FFixedString Path = Paths::Combine(Folder, Name);
        Path = VFS::ResolveToVirtualPath(Path);
        if (Path.empty() || Path.front() != '/')
        {
            OutError = "Folder must resolve to a virtual path under a mounted alias (e.g. /Game).";
            return nullptr;
        }
        // Free of loaded packages and in-flight imports as well as files, since CreatePackage refuses all three.
        Path = Import::PathReservations::FindFree(Path);
        if (Path.empty())
        {
            OutError = "No free name in that folder.";
            return nullptr;
        }
        CPackage::AddPackageExt(Path);

        CPackage* NewPackage = CPackage::CreatePackage(Path);
        if (NewPackage == nullptr) { OutError = "Failed to create package."; return nullptr; }

        const FStringView FileName = VFS::FileName(Path, true);
        CPrefab* Prefab = NewObject<CPrefab>(NewPackage, FName(FileName));
        if (Prefab == nullptr) { OutError = "Failed to create prefab object."; return nullptr; }
        Prefab->SetFlag(OF_Public);

        // A scratch parent at the pivot, reparented world-preserving, then captured and restored.
        ECS::FEntity CaptureRoot = ECS::NullEntity;
        ECS::FEntity ScratchRoot = ECS::NullEntity;
        TVector<ECS::FEntity> OriginalParents;

        if (Roots.size() == 1)
        {
            CaptureRoot = Roots[0];
        }
        else
        {
            FTransform PivotTransform;
            PivotTransform.SetLocation(Pivot);
            ScratchRoot = World->ConstructEntity(FName(FileName), PivotTransform);
            if (ScratchRoot == ECS::NullEntity)
            {
                OutError = "Failed to create scratch parent.";
                return nullptr;
            }

            OriginalParents.reserve(Roots.size());
            for (ECS::FEntity Entity : Roots)
            {
                OriginalParents.push_back(WorkingRegistry.GetHierarchy().GetParent(Entity));
                ECS::Utils::ReparentEntity(WorkingRegistry, Entity, ScratchRoot);
            }
            CaptureRoot = ScratchRoot;
        }

        Prefab->CaptureFromWorld(World.Get(), CaptureRoot);

        // Anchor the captured root at origin so the prefab opens centered in its editor.
        Prefab->Registry.ForEachEntity([&](ECS::FEntity E)
        {
            if (Prefab->Registry.GetHierarchy().GetParent(E) != ECS::NullEntity)
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
            for (size_t i = 0; i < Roots.size(); ++i)
            {
                if (OriginalParents[i] == ECS::NullEntity)
                {
                    ECS::Utils::RemoveFromParent(WorkingRegistry, Roots[i]);
                }
                else
                {
                    ECS::Utils::ReparentEntity(WorkingRegistry, Roots[i], OriginalParents[i]);
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

        if (!CPackage::SavePackage(NewPackage, Path))
        {
            OutError = "Failed to save prefab to disk.";
            return nullptr;
        }
        FAssetRegistry::Get().AssetCreated(Prefab);
        return Prefab;
    }

    CPrefab* FWorldEditorTool::CreatePrefabFromSelection(FStringView Folder, FStringView Name, FFixedString& OutError)
    {
        if (World == nullptr || World->IsSimulating())
        {
            OutError = "Cannot create a prefab while simulating.";
            return nullptr;
        }
        TVector<ECS::FEntity> Roots = GatherPrefabSelectionRoots();
        if (Roots.empty())
        {
            OutError = "Nothing selected that a prefab can capture.";
            return nullptr;
        }
        const FCreatePrefabRequest Req = BuildCreatePrefabRequest(World->GetEntityRegistry(), Move(Roots));
        return CreatePrefabFromRoots(Req.Roots, Req.Pivot, Folder, Name, OutError);
    }

    void FWorldEditorTool::PushCreatePrefabModalForEntity(ECS::FEntity Entity)
    {
        if (World == nullptr || World->IsSimulating())
        {
            ImGuiX::Notifications::NotifyWarning("Cannot create a prefab while simulating.");
            return;
        }

        ECS::FRegistry& Registry = World->GetEntityRegistry();
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

        ECS::FRegistry& Registry = World->GetEntityRegistry();
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
        Source->RefreshInstance(World.Get(), InstanceRoot);
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
        if (CPrefab::DetachInstance(World.Get(), InstanceRoot))
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
            ECS::FRegistry& OldRegistry = World->GetEntityRegistry();
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
            PossessedCameraEntity = World->GetCamera()->GetActiveCameraEntity();

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

            if (InputViewport)
            {
                PossessedInputMode = InputViewport->GetContext().GetInputMode();
                PossessedMouseMode = InputViewport->GetContext().GetMouseMode();
                bHasPossessedInputModes = true;
            }

            // Set last, since HasEditorCameraControl flips with it and the refocus handler reads it next draw.
            bEjectedFromPlay = true;
            FInputViewportRegistry::Get().SetGameInputFocused(false);
        }
        else
        {
            // Re-activated first, so the world is never left with a resolved view on a dead entity.
            if (PossessedCameraEntity != ECS::NullEntity && World->IsValidEntity(PossessedCameraEntity))
            {
                World->GetCamera()->SetActiveCamera(PossessedCameraEntity);
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

            // Before focus returns, so the game gets back the cursor it had rather than whatever the flycam left.
            if (bHasPossessedInputModes && InputViewport)
            {
                InputViewport->GetContext().SetInputMode(PossessedInputMode);
                FInputViewportRegistry::Get().SetMouseMode(PossessedMouseMode, World.Get());
            }
            bHasPossessedInputModes = false;

            bEjectedFromPlay = false;
            FInputViewportRegistry::Get().SetGameInputFocused(true);
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
            bHasPossessedInputModes = false;
        }
    }

    void FWorldEditorTool::StopAllSimulations()
    {
        SetWorldNewSimulate(false);
        SetWorldPlayInEditor(false);
    }

    bool FWorldEditorTool::StartPlayInEditor()
    {
        if (HasSimulatingWorld())
        {
            return false;
        }

        SetWorldPlayInEditor(true);
        return true;
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
        // An undo after a save changes the level again, so the next save must not skip it.
        MarkSceneDirty();

        // A prefab update is the asset's, so stepping back past one must not leave its copies on the older data.
        CPrefab::RefreshStaleInstances(World.Get());

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
            auto TerrainView = World->GetEntityRegistry().View<STerrainComponent>();
            for (ECS::FEntity Entity : TerrainView)
            {
                FTerrainCPUState& State = World->GetEntityRegistry().Get<STerrainComponent>(Entity).CPUState;
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
        const DefaultScene::FCameraPose Pose = DefaultScene::GetWelcomeCameraPose();
        OutLocation = Pose.Location;
        OutTarget   = Pose.Target;
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

        ECS::FRegistry& Registry = GetObservedWorld()->GetEntityRegistry();
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
            // Before StartPIE, which logs the session's first lines.
            if (GetDefault<CWorldEditorSettings>()->bClearConsoleOnPIE)
            {
                Logging::ClearLogQueue();
            }

            bGamePreviewRunning = true;
            bGameViewMode = true; // default to a clean game view on play; toggle off to see editor/debug overlays
            PropertyTables.clear();
            SelectedEntities.clear();
            LastSelectedEntity = ECS::NullEntity;
            DetailsEntity = ECS::NullEntity;
            bDetailsDirty = true;

            // Leftover tags on ProxyWorld confuse the outliner rebuild on PIE-exit.
            World->GetEntityRegistry().ClearComponent<FSelectedInEditorComponent>();
            World->GetEntityRegistry().ClearComponent<FLastSelectedTag>();

            World->SetActive(false);
            ProxyWorld = World;
            ProxyEditorEntity = EditorEntity;

            // Its net mode makes CreateRenderer a no-op, so it stays invisible and listens on loopback.
            if (PlaySettings.NetMode == ENetMode::DedicatedServer)
            {
                PIEDedicatedServerWorld = GWorldManager->StartPIE(ProxyWorld.Get(), EWorldType::Game, ENetMode::DedicatedServer);
                if (PIEDedicatedServerWorld == nullptr)
                {
                    LOG_WARN("Failed to start the dedicated-server world for PIE.");
                }
            }

            // StartPIE returns null when the world cannot be duplicated, so bail before rebinding to null.
            CWorld* PIEWorld = GWorldManager->StartPIE(ProxyWorld.Get(), EWorldType::Game, ResolvePlayerNetMode(0));
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
            FInputViewportRegistry::Get().SetGameInputFocused(true);

            // FEditorUI spawns the other players' PIE worlds and pop-ups through a deferred tool create.
            OnGamePreviewStartRequested.Broadcast();
            FCoreEditorDelegates::OnPIEBegin.Broadcast(PIEWorld);
        }
        else
        {
            // Broadcast while the PIE world is still live; the teardown below invalidates it.
            FCoreEditorDelegates::OnPIEEnd.Broadcast(World.Get());

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
            bHasPossessedInputModes = false;

            PropertyTables.clear();
            ResetSelectionState();
            DetailsEntity = ECS::NullEntity;
            bDetailsDirty = true;
            World->SetPaused(true);
            bGamePreviewRunning = false;
            bGameViewMode = false; // back to the editor view on stop

            // Hand input back to the editor on stop.
            FInputViewportRegistry::Get().SetGameInputFocused(false);

            // SetupWorldForTool builds a fresh editor entity at origin, so stash the pre-Play camera pose.
            bool bHasSavedCamera = false;
            FTransform SavedCameraTransform;
            SCameraComponent SavedCamera;
            // Use ProxyEditorEntity (captured at PIE entry); EditorEntity may be null if Travel swapped worlds mid-PIE.
            if (ProxyEditorEntity != ECS::NullEntity && ProxyWorld->GetEntityRegistry().IsValid(ProxyEditorEntity))
            {
                SavedCameraTransform = ProxyWorld->GetEntityRegistry().Get<STransformComponent>(ProxyEditorEntity).GetWorldTransform();
                SavedCamera = ProxyWorld->GetEntityRegistry().Get<SCameraComponent>(ProxyEditorEntity);
                bHasSavedCamera = true;

                ProxyWorld->DestroyEntity(ProxyEditorEntity);
            }
            ProxyEditorEntity = ECS::NullEntity;
            EditorEntity = ECS::NullEntity;

            SetWorld(ProxyWorld.Get());
            ProxyWorld->SetActive(true);

            if (bHasSavedCamera && World->GetEntityRegistry().IsValid(EditorEntity))
            {
                World->GetEntityRegistry().Get<STransformComponent>(EditorEntity).SetLocalTransform(SavedCameraTransform);
                World->GetEntityRegistry().Patch<SCameraComponent>(EditorEntity, [SavedCamera](SCameraComponent& Patch)
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
                // Activate the editor viewport first so the mode change lands on the right context.
                FInputViewportRegistry::Get().SetActiveViewport(InputViewport.get());

                InputViewport->GetContext().SetInputMode(EInputMode::Game);

                // Through the registry so ImGuiConfigFlags_NoMouse clears, which a direct context write would leave set.
                FInputViewportRegistry::Get().SetMouseMode(EMouseMode::Normal);
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
            World->GetEntityRegistry().ClearComponent<FSelectedInEditorComponent>();
            World->GetEntityRegistry().ClearComponent<FLastSelectedTag>();

            FTransform TransformCopy = World->GetEntityRegistry().Get<STransformComponent>(EditorEntity).GetWorldTransform();
            SCameraComponent CameraCopy = World->GetEntityRegistry().Get<SCameraComponent>(EditorEntity);

            World->SetActive(false);
            ProxyWorld = World;
            ProxyEditorEntity = EditorEntity;

            // StartPIE returns null when the world cannot be duplicated, so bail before dereferencing it.
            CWorld* SimWorld = GWorldManager->StartPIE(ProxyWorld.Get(), EWorldType::Simulation, ENetMode::Standalone);
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

            if (ProxyEditorEntity != ECS::NullEntity && ProxyWorld->GetEntityRegistry().IsValid(ProxyEditorEntity))
            {
                ProxyWorld->DestroyEntity(ProxyEditorEntity);
            }
            ProxyEditorEntity = ECS::NullEntity;
            EditorEntity = ECS::NullEntity;

            SetupWorldForTool();
            ASSERT(World->GetEntityRegistry().IsValid(EditorEntity));

            WorldSettingsPropertyTable = MakeUnique<FPropertyTable>(&World->GetDefaultWorldSettings(), SDefaultWorldSettings::StaticStruct());

            World->GetEntityRegistry().Get<STransformComponent>(EditorEntity).SetLocalTransform(TransformCopy);

            World->GetEntityRegistry().Patch<SCameraComponent>(EditorEntity, [CameraCopy](SCameraComponent& Patch)
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

            FTransform TransformCopy = World->GetEntityRegistry().Get<STransformComponent>(EditorEntity).GetWorldTransform();
            SCameraComponent CameraCopy = World->GetEntityRegistry().Get<SCameraComponent>(EditorEntity);

            if (EditorEntity != ECS::NullEntity && World->GetEntityRegistry().IsValid(EditorEntity))
            {
                World->DestroyEntity(EditorEntity);
            }
            EditorEntity = ECS::NullEntity;
            ProxyEditorEntity = ECS::NullEntity;

            SetWorld(ProxyWorld.Get());
            ProxyWorld->SetActive(true);
            ASSERT(World->GetEntityRegistry().IsValid(EditorEntity));

            WorldSettingsPropertyTable = MakeUnique<FPropertyTable>(&World->GetDefaultWorldSettings(), SDefaultWorldSettings::StaticStruct());

            World->GetEntityRegistry().Get<STransformComponent>(EditorEntity).SetLocalTransform(TransformCopy);

            World->GetEntityRegistry().Patch<SCameraComponent>(EditorEntity, [CameraCopy](SCameraComponent& Patch)
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
                FInputViewportRegistry::Get().SetMouseMode(EMouseMode::Normal);
            }
        }
    }

    bool FWorldEditorTool::OnEvent(FEvent& Event)
    {
        // Game focus hides the key from ImGui, and stopping tears down the world, so it defers to Update.
        if (bGamePreviewRunning && Event.IsA<FKeyPressedEvent>())
        {
            FKeyPressedEvent& Key = Event.As<FKeyPressedEvent>();
            if (Key.GetKeyCode() == EKey::Escape && Key.IsModifierDown(EKey::LeftShift) && !Key.IsRepeat())
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
                FString Refusal;
                if (!ReparentEntityTransacted(SourceEntity, DropItem, true, &Refusal))
                {
                    ImGuiX::Notifications::NotifyError("{}", Refusal);
                }
            }
            return;
        }

        AcceptContentBrowserPayload(DropItem, /*bAttachToTarget*/ true);
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
        AcceptContentBrowserPayload(ECS::NullEntity, /*bAttachToTarget*/ false);
        PendingAssetDropFolder = 0;
    }

    void FWorldEditorTool::HandlePrefabContentDrop(FStringView VirtualPath, ECS::FEntity DropTarget, bool bAttachToTarget, const FTransform* SpawnTransform)
    {
        BeginTransaction();
        ECS::FEntity Spawned = HandleContentBrowserAssetDrop(VirtualPath, DropTarget, bAttachToTarget, SpawnTransform);
        if (Spawned != ECS::NullEntity)
        {
            // Filed inside the same transaction so the drop is one undo step, not two.
            if (Spawned != DropTarget)
            {
                OnDropSpawnedEntity(Spawned);
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

    void FWorldEditorTool::OnDropSpawnedEntity(ECS::FEntity Entity)
    {
        if (PendingAssetDropFolder == 0)
        {
            return;
        }
        if (SSceneFolderComponent* Folders = GetEditableSceneFolders())
        {
            Folders->AssignEntity(Entity, PendingAssetDropFolder);
            EntityFolderCache[Entity] = PendingAssetDropFolder;
        }
    }

    void FWorldEditorTool::DrawWorldSettings(bool bFocused)
    {
        WorldSettingsPropertyTable->DrawTree();
    }

    // Lists up to three stages, otherwise a count.
    namespace
    {
        struct FSystemStageDesc
        {
            const char* Label;
            const char* Description;
        };

        // Indexed by EUpdateStage, and listed in the order a frame runs them, with the editor-only stage last.
        constexpr FSystemStageDesc kSystemStages[] =
        {
            { "Frame Start",    "Runs first each frame, before physics steps." },
            { "Pre Physics",    "Runs just before physics steps, which is where movement and forces are written." },
            { "During Physics", "Runs while the physics step is in flight on its own thread." },
            { "Post Physics",   "Runs once physics results are in, which is where gameplay reads them back." },
            { "Frame End",      "Runs last each frame, before the world is handed to the renderer." },
            { "Paused",         "Runs instead of the gameplay stages while the world is not simulating, which is how systems keep working in the editor." },
            { "No Update",      "Asks for no stage, so it only runs its startup and teardown, and whatever those set going." },
        };
        static_assert(std::size(kSystemStages) == (size_t)EUpdateStage::Max + 1, "One description per update stage, then the systems in none.");

        // Not a real stage, so a system that ticks nowhere still gets a row to be seen and turned off from.
        constexpr uint8 kNoUpdateStage = (uint8)EUpdateStage::Max;

        // How long the panel trusts its list of system classes, since gathering it configures every one of them.
        constexpr double kSystemListRefreshSeconds = 1.0;

        // "SBlockoutSystem" reads as "Blockout", and a C# "Game.WaveSystem" as "Wave".
        FString FriendlySystemName(const FString& ClassName)
        {
            FStringView Name(ClassName);
            if (const size_t Dot = Name.find_last_of('.'); Dot != FStringView::npos)
            {
                Name = Name.substr(Dot + 1);
            }
            if (Name.size() > 1 && Name[0] == 'S' && Name[1] >= 'A' && Name[1] <= 'Z')
            {
                Name.remove_prefix(1);
            }
            if (Name.size() > 6 && Name.ends_with("System"))
            {
                Name.remove_suffix(6);
            }

            FString Spaced;
            for (size_t Index = 0; Index < Name.size(); ++Index)
            {
                const char Char = Name[Index];
                const bool bUpper = Char >= 'A' && Char <= 'Z';
                const bool bAfterLower = Index > 0 && Name[Index - 1] >= 'a' && Name[Index - 1] <= 'z';
                if (bUpper && bAfterLower)
                {
                    Spaced.push_back(' ');
                }
                Spaced.push_back(Char);
            }
            return Spaced.empty() ? ClassName : Spaced;
        }

        // A component's access entry without the "S" and "Component" its type name carries.
        FString FriendlyAccessName(uint32 Id)
        {
            FString Name = GetAccessTypeName(Id).ToString();
            if (Name.size() > 1 && Name[0] == 'S' && Name[1] >= 'A' && Name[1] <= 'Z')
            {
                Name.erase(0, 1);
            }
            constexpr FStringView Suffix("Component");
            if (Name.size() > Suffix.size() && FStringView(Name).ends_with(Suffix))
            {
                Name.resize(Name.size() - Suffix.size());
            }
            return Name;
        }

        // A small rounded label, laid out inline and wrapped onto the next line when the row is full.
        void DrawSystemChip(const FString& Text, const ImVec4& TextColor, const ImVec4& Fill)
        {
            const ImVec2 Padding(6.0f, 2.0f);
            const ImVec2 Size = ImGui::CalcTextSize(Text.c_str()) + Padding * 2.0f;
            if (ImGui::GetContentRegionAvail().x < Size.x)
            {
                ImGui::NewLine();
            }

            const ImVec2 Min = ImGui::GetCursorScreenPos();
            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            DrawList->AddRectFilled(Min, Min + Size, EditorColors::U32(Fill), Size.y * 0.5f);
            DrawList->AddText(Min + Padding, EditorColors::U32(TextColor), Text.c_str());
            ImGui::Dummy(Size);
            ImGui::SameLine(0.0f, 4.0f);
        }
    }

    void FWorldEditorTool::DrawSystemsPanel(bool bFocused)
    {
        if (World == nullptr)
        {
            return;
        }

        const double Now = ImGui::GetTime();
        if (CachedSystemsWorld != World.Get() || CachedSystemInfos.empty() || Now - CachedSystemsTime > kSystemListRefreshSeconds)
        {
            World->GetAllSystems(CachedSystemInfos);
            CachedSystemsWorld = World.Get();
            CachedSystemsTime  = Now;
        }

        TVector<FSystemScheduleEntry> Schedule;
        World->GetSystemSchedule(Schedule);

        auto FindInfo = [this](FName Name) -> const CWorld::FSystemInfo*
        {
            for (const CWorld::FSystemInfo& Info : CachedSystemInfos)
            {
                if (Info.Name == Name)
                {
                    return &Info;
                }
            }
            return nullptr;
        };

        int32 EnabledCount = 0;
        for (const CWorld::FSystemInfo& Info : CachedSystemInfos)
        {
            EnabledCount += Info.bEnabled ? 1 : 0;
        }

        ImGuiX::SearchBar("##SystemSearch", SystemsFilter, LE_ICON_MAGNIFY " Search systems...");

        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
        const int32 DisabledCount = (int32)CachedSystemInfos.size() - EnabledCount;
        if (DisabledCount > 0)
        {
            ImGui::Text("%d systems, %d turned off in this world", (int32)CachedSystemInfos.size(), DisabledCount);
        }
        else
        {
            ImGui::Text("%d systems, none turned off", (int32)CachedSystemInfos.size());
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGuiX::HelpMarker("Systems tick in the stages below, top to bottom, in the order listed. Untick one to stop it in this "
            "world. The change takes effect next frame and saves with the world. Select a system to see what it touches.");

        const CWorld::FSystemInfo* Selected = SelectedSystem.IsNone() ? nullptr : FindInfo(SelectedSystem);
        const float DetailHeight = Selected != nullptr ? ImGui::GetContentRegionAvail().y * 0.4f : 0.0f;

        bool bToggled = false;
        if (ImGui::BeginChild("##SystemsList", ImVec2(0.0f, -DetailHeight), ImGuiChildFlags_None))
        {
            bool bAnyRow = false;
            for (uint8 Stage = 0; Stage <= kNoUpdateStage; ++Stage)
            {
                // Scheduled systems in the order they run, then this stage's turned-off systems, which have no slot.
                struct FStageRow
                {
                    const CWorld::FSystemInfo*   Info = nullptr;
                    const FSystemScheduleEntry*  Entry = nullptr;
                };
                TVector<FStageRow> Rows;
                for (const FSystemScheduleEntry& Entry : Schedule)
                {
                    const CWorld::FSystemInfo* Info = Entry.Stage == Stage ? FindInfo(Entry.Name) : nullptr;
                    if (Info != nullptr && ImGuiX::PassSearchFilter(SystemsFilter, FriendlySystemName(Info->Name.ToString()).c_str()))
                    {
                        Rows.push_back({ Info, &Entry });
                    }
                }
                const bool bStageRunning = Algo::AnyOf(Schedule, [Stage](const FSystemScheduleEntry& Entry) { return Entry.Stage == Stage; });

                // An editor world only schedules the paused stage, so the gameplay stages list what will run once play starts.
                const size_t FirstUnscheduled = Rows.size();
                for (const CWorld::FSystemInfo& Info : CachedSystemInfos)
                {
                    const bool bInStage = Stage == kNoUpdateStage ? Info.Stages.empty() : Algo::Contains(Info.Stages, (EUpdateStage)Stage);
                    const bool bScheduled = Algo::AnyOf(Rows, [&Info](const FStageRow& Row) { return Row.Info == &Info; });
                    if (bInStage && !bScheduled && ImGuiX::PassSearchFilter(SystemsFilter, FriendlySystemName(Info.Name.ToString()).c_str()))
                    {
                        Rows.push_back({ &Info, nullptr });
                    }
                }
                Algo::Sort(Rows.begin() + FirstUnscheduled, Rows.end(), [](const FStageRow& A, const FStageRow& B)
                {
                    return FriendlySystemName(A.Info->Name.ToString()) < FriendlySystemName(B.Info->Name.ToString());
                });
                if (Rows.empty())
                {
                    continue;
                }
                bAnyRow = true;

                ImGui::PushID(Stage);
                const float HeaderRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 6.0f));
                ImGui::PushStyleColor(ImGuiCol_Header, EditorColors::ComponentHeader());
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorColors::Lighten(EditorColors::ComponentHeader(), 0.05f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorColors::Lighten(EditorColors::ComponentHeader(), 0.08f));
                const FString HeaderLabel = Format("{}  {}###Stage", LE_ICON_TIMELINE_CLOCK_OUTLINE, kSystemStages[Stage].Label);
                const bool bOpen = ImGui::CollapsingHeader(HeaderLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar();
                ImGuiX::TextTooltip("{}", bStageRunning || Stage == (uint8)EUpdateStage::Paused || Stage == kNoUpdateStage
                    ? FString(kSystemStages[Stage].Description)
                    : Format("{} Nothing runs here until the world simulates.", kSystemStages[Stage].Description));

                const FString Count = bStageRunning || Stage == (uint8)EUpdateStage::Paused || Stage == kNoUpdateStage
                    ? Format("{}", Rows.size())
                    : Format("{}  {} play only", Rows.size(), LE_ICON_PLAY);
                ImGui::SameLine(HeaderRight - ImGui::CalcTextSize(Count.c_str()).x - ImGui::GetStyle().FramePadding.x * 2.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(EditorColors::TextMuted(), "%s", Count.c_str());

                if (bOpen)
                {
                    for (const FStageRow& Row : Rows)
                    {
                        const CWorld::FSystemInfo& Info = *Row.Info;
                        const FString Friendly = FriendlySystemName(Info.Name.ToString());
                        const bool bManaged = Row.Entry != nullptr ? Row.Entry->bManaged : Info.Name.ToString().find('.') != FString::npos;

                        ImGui::PushID(Info.Name.c_str());
                        bool bEnabled = Info.bEnabled;
                        if (ImGui::Checkbox("##Enabled", &bEnabled))
                        {
                            World->SetSystemEnabled(Info.Class, bEnabled);
                            MarkSceneDirty();
                            bToggled = true;
                        }
                        ImGuiX::TextTooltip("{}", Info.bEnabled
                            ? Format("Turn {} off in this world.", Friendly)
                            : Format("Turn {} back on in this world.", Friendly));

                        ImGui::SameLine();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(bManaged ? EditorColors::AccentAlt() : EditorColors::TextDim(), "%s",
                            bManaged ? LE_ICON_LANGUAGE_CSHARP : LE_ICON_LANGUAGE_CPP);
                        ImGui::SameLine();

                        // The badge column is measured first, so a long name gives way to it rather than running under it.
                        FString Badge;
                        ImVec4 BadgeColor = EditorColors::TextMuted();
                        const char* BadgeHelp = nullptr;
                        if (!Info.bEnabled)
                        {
                            Badge = "Off";
                            BadgeHelp = "Turned off in this world, so it does not tick.";
                        }
                        else if (Row.Entry != nullptr && Row.Entry->BatchSize > 1)
                        {
                            Badge = Format("{} Parallel", LE_ICON_SOURCE_BRANCH);
                            BadgeColor = EditorColors::Success();
                            BadgeHelp = "Runs at the same time as the other systems in its batch, since none of them touch what it writes.";
                        }
                        else if (Row.Entry != nullptr && Row.Entry->bExclusive)
                        {
                            Badge = Format("{} Exclusive", LE_ICON_LOCK);
                            BadgeHelp = "Declares no component access, so it runs alone. Declaring reads and writes in Configure lets it run alongside others.";
                        }

                        const float BadgeWidth = Badge.empty() ? 0.0f : ImGui::CalcTextSize(Badge.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
                        const float NameWidth = Math::Max(ImGui::GetContentRegionAvail().x - BadgeWidth, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, Info.bEnabled ? EditorColors::TextPrimary() : EditorColors::TextMuted());
                        if (ImGui::Selectable(Friendly.c_str(), SelectedSystem == Info.Name, ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(NameWidth, ImGui::GetFrameHeight())))
                        {
                            SelectedSystem = SelectedSystem == Info.Name ? FName() : Info.Name;
                        }
                        ImGui::PopStyleColor();
                        ImGuiX::TextTooltip("{}", Info.Name.ToString());

                        if (!Badge.empty())
                        {
                            ImGui::SameLine();
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextColored(BadgeColor, "%s", Badge.c_str());
                            ImGuiX::TextTooltip("{}", BadgeHelp);
                        }
                        ImGui::PopID();
                    }
                    ImGui::Spacing();
                }
                ImGui::PopID();
            }

            if (!bAnyRow)
            {
                ImGui::Spacing();
                ImGui::TextColored(EditorColors::TextMuted(), "%s", CachedSystemInfos.empty() ? "This world has no systems." : "No systems match the search.");
            }
        }
        ImGui::EndChild();

        if (bToggled)
        {
            World->GetAllSystems(CachedSystemInfos);
            CachedSystemsTime = Now;
        }

        if (Selected != nullptr)
        {
            DrawSelectedSystemDetails(*Selected, Schedule);
        }
    }

    void FWorldEditorTool::DrawSelectedSystemDetails(const CWorld::FSystemInfo& Info, const TVector<FSystemScheduleEntry>& Schedule)
    {
        if (!ImGui::BeginChild("##SystemDetails", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
        {
            ImGui::EndChild();
            return;
        }

        const CClass* Class = FindObject<CClass>(Info.Name);
        const bool bManaged = Info.Name.ToString().find('.') != FString::npos;

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
        ImGui::TextUnformatted(FriendlySystemName(Info.Name.ToString()).c_str());
        ImGuiX::Font::PopFont();
        ImGui::TextColored(EditorColors::TextMuted(), "%s  %s", bManaged ? "C# system" : "C++ system", Info.Name.c_str());

        if (Class != nullptr && Class->HasMeta("ToolTip"))
        {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(EditorColors::TextDim(), "%s", Class->GetMeta("ToolTip").c_str());
            ImGui::PopTextWrapPos();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        auto Field = [](const char* Label)
        {
            ImGui::TextColored(EditorColors::TextMuted(), "%s", Label);
        };

        Field("Runs in");
        if (!Info.bEnabled)
        {
            ImGui::TextColored(EditorColors::Warning(), "Turned off in this world");
        }
        ImGui::PushTextWrapPos(0.0f);
        for (size_t StageIndex = 0; StageIndex < Info.Stages.size(); ++StageIndex)
        {
            const EUpdateStage Stage = Info.Stages[StageIndex];
            const FSystemScheduleEntry* Entry = nullptr;
            for (const FSystemScheduleEntry& Candidate : Schedule)
            {
                if (Candidate.Name == Info.Name && Candidate.Stage == (uint8)Stage)
                {
                    Entry = &Candidate;
                    break;
                }
            }

            FString Line = kSystemStages[(uint8)Stage].Label;
            if (Entry != nullptr)
            {
                Line += Entry->BatchSize > 1
                    ? Format(", batch {} alongside {} other{}", Entry->Batch + 1, Entry->BatchSize - 1, Entry->BatchSize == 2 ? "" : "s")
                    : Format(", batch {} on its own", Entry->Batch + 1);
            }
            ImGui::Bullet();
            ImGui::TextWrapped("%s", Line.c_str());
        }
        ImGui::PopTextWrapPos();

        auto DrawOrder = [](const char* Label, const TVector<FName>& Systems)
        {
            if (Systems.empty())
            {
                return;
            }
            ImGui::Spacing();
            ImGui::TextColored(EditorColors::TextMuted(), "%s", Label);
            for (FName System : Systems)
            {
                DrawSystemChip(FriendlySystemName(System.ToString()), EditorColors::TextPrimary(), EditorColors::WithAlpha(EditorColors::TextMuted(), 0.25f));
            }
            ImGui::NewLine();
        };
        DrawOrder("Runs after", Info.RunsAfter);
        DrawOrder("Runs before", Info.RunsBefore);

        const FSystemScheduleEntry* Any = nullptr;
        for (const FSystemScheduleEntry& Candidate : Schedule)
        {
            if (Candidate.Name == Info.Name)
            {
                Any = &Candidate;
                break;
            }
        }

        ImGui::Spacing();
        Field("Writes");
        if (Any == nullptr)
        {
            ImGui::TextColored(EditorColors::TextMuted(), "Shown once the system is running.");
        }
        else if (Any->bExclusive)
        {
            ImGui::TextColored(EditorColors::TextDim(), "Everything. It declares no access, so it runs alone.");
        }
        else
        {
            for (uint32 Id : Any->Writes)
            {
                DrawSystemChip(FriendlyAccessName(Id), EditorColors::TextPrimary(), EditorColors::WithAlpha(EditorColors::Warning(), 0.25f));
            }
            if (Any->Writes.empty())
            {
                ImGui::TextColored(EditorColors::TextMuted(), "Nothing");
            }
            ImGui::NewLine();

            Field("Reads");
            for (uint32 Id : Any->Reads)
            {
                DrawSystemChip(FriendlyAccessName(Id), EditorColors::TextPrimary(), EditorColors::WithAlpha(EditorColors::Accent(), 0.25f));
            }
            if (Any->Reads.empty())
            {
                ImGui::TextColored(EditorColors::TextMuted(), "Nothing");
            }
            ImGui::NewLine();
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

        AcceptContentBrowserPayload(ECS::NullEntity, /*bAttachToTarget*/ false);
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
        ECS::FRegistry& Registry = World->GetEntityRegistry();
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

            if (const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(Entity); Parent != ECS::NullEntity)
            {
                const FMatrix4 ParentWorld = Registry.Get<STransformComponent>(Parent).GetWorldMatrix();
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

        ECS::FRegistry& Registry = World->GetEntityRegistry();

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

        ECS::FRegistry& Registry = World->GetEntityRegistry();
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

        ECS::FRegistry& Registry = World->GetEntityRegistry();
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

        ECS::FRegistry& Registry = World->GetEntityRegistry();

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
            Registry.GetHierarchy().ForEachDescendant(Entity, [&](ECS::FEntity Desc)
            {
                Exclude.insert(Desc);
            });
        }

        // Snapshot world-space AABBs once so per-entity raycast is a flat vector walk.
        struct FCandidate { SAABB Box; };
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

        for (ECS::FEntity Entity : Targets)
        {
            STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
            const FVector3 WorldLocation = Transform.GetWorldLocation();
            const FVector3 Origin = WorldLocation + FVector3(0.0f, 0.5f, 0.0f);

            float BestT = FLT_MAX;
            for (const FCandidate& C : Candidates)
            {
                float T;
                if (CastDownToAABB(Origin, C.Box, T) && T < BestT)
                {
                    BestT = T;
                }
            }

            // Fallback to Y=0 plane so the action always does something predictable.
            float NewY;
            if (BestT < FLT_MAX)
            {
                NewY = Origin.y - BestT;
            }
            else
            {
                NewY = 0.0f;
            }

            FTransform NewWorld = Transform.GetWorldTransform();
            FVector3 NewLoc = NewWorld.GetLocation();
            NewLoc.y = NewY;
            NewWorld.SetLocation(NewLoc);

            if (const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(Entity); Parent != ECS::NullEntity)
            {
                FMatrix4 ParentWorld = Registry.Get<STransformComponent>(Parent).GetWorldMatrix();
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

        ECS::FRegistry& Registry = World->GetEntityRegistry();
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
        ECS::FRegistry& Registry = World->GetEntityRegistry();
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

        ECS::FRegistry& Registry = World->GetEntityRegistry();

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

            if (const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(Entity); Parent != ECS::NullEntity)
            {
                FMatrix4 ParentWorld = Registry.Get<STransformComponent>(Parent).GetWorldMatrix();
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

        ECS::FRegistry& Registry = World->GetEntityRegistry();
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

        ECS::FRegistry& Registry = World->GetEntityRegistry();
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

        ECS::FRegistry& Registry = World->GetEntityRegistry();
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

        ECS::FRegistry& Registry = World->GetEntityRegistry();
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

    void FWorldEditorTool::SetPlayNetwork(int32 NumPlayers, ENetMode NetMode)
    {
        PlaySettings.NumPlayers = Math::Clamp(NumPlayers, 1, MaxPlayers);
        PlaySettings.NetMode    = NetMode;
    }
}
