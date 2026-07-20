#include "SkeletonEditorTool.h"
#include "ImGuiDrawUtils.h"
#include "assets/assettypes/mesh/skeleton/skeleton.h"
#include "Core/Math/Math.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/Customizations/BonePickerContext.h"
#include "world/entity/components/environmentcomponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "world/entity/components/skeletalmeshcomponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"


namespace Lumina
{
    static const char* SkeletonPropertiesName       = "Skeleton Properties";
    static const char* BoneHierarchyName            = "Bone Hierarchy";

    FSkeletonEditorTool::FSkeletonEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    void FSkeletonEditorTool::OnInitialize()
    {
        CreateToolWindow(SkeletonPropertiesName, [&](bool bFocused)
        {
            CSkeleton* Skeleton = GetAsset<CSkeleton>();
            FSkeletonResource* SkeletonResource = Skeleton->GetSkeletonResource();

            // Socket BoneName fields get the bone-tree popup.
            BonePickerContext::FScope BonePickerScope(SkeletonResource);

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Asset Details");
            ImGuiX::Font::PopFont();
            
            ImGui::Spacing();

            ImGui::Text("Number of Bones: %i", SkeletonResource->GetNumBones());
            ImGui::Text("Skeleton Name: %s", SkeletonResource->Name.ToString().c_str());
            
            ImGui::Spacing();
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Statistics");
            ImGuiX::Font::PopFont();
            
            int32 NumRootBones = 0;
            int32 MaxDepth = 0;
            int32 TotalDepth = 0;
            
            for (const auto& Bone : SkeletonResource->Bones)
            {
                if (Bone.ParentIndex == -1)
                {
                    NumRootBones++;
                }
                
                int32 Depth = 0;
                int32 CurrentIndex = &Bone - SkeletonResource->Bones.data();
                while (SkeletonResource->Bones[CurrentIndex].ParentIndex != -1)
                {
                    CurrentIndex = SkeletonResource->Bones[CurrentIndex].ParentIndex;
                    Depth++;
                }
                MaxDepth = eastl::max(MaxDepth, Depth);
                TotalDepth += Depth;
            }
            
            float AvgDepth = SkeletonResource->GetNumBones() > 0 ? (float)TotalDepth / SkeletonResource->GetNumBones() : 0.0f;
            
            ImGui::Text("Root Bones: %i", NumRootBones);
            ImGui::Text("Maximum Hierarchy Depth: %i", MaxDepth);
            ImGui::Text("Average Bone Depth: %.2f", AvgDepth);
            
            ImGui::Spacing();
            PropertyTable.DrawTree();
        });
        
        CreateToolWindow(BoneHierarchyName, [&](bool bFocused)
        {
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Bone Hierarchy");
            ImGuiX::Font::PopFont();
            ImGui::Spacing();
            
            BoneListView.Draw(BoneListContext);
        });
        
        
                
        BoneListContext.ItemContextMenuFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            if (!Item.IsValid())
            {
                return;
            }

            const FName BoneName = Tree.Get<FTreeNodeDisplay>(Item).DisplayName;

            if (ImGui::MenuItem(LE_ICON_PLUS " Add Socket"))
            {
                CSkeleton* Skeleton = GetAsset<CSkeleton>();

                FString Base(BoneName.c_str());
                Base += "_Socket";
                FName SocketName(Base.c_str());
                int32 Suffix = 1;
                while (Skeleton->FindSocket(SocketName) != nullptr)
                {
                    FString Numbered = Base;
                    Numbered += "_";
                    Numbered += eastl::to_string(Suffix++).c_str();
                    SocketName = FName(Numbered.c_str());
                }

                FMeshSocket& Socket = Skeleton->Sockets.emplace_back();
                Socket.SocketName = SocketName;
                Socket.BoneName = BoneName;

                Asset->GetPackage()->MarkDirty();
                PropertyTable.MarkDirty();
            }
        };

        BoneListContext.DragDropFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {

        };

        BoneListContext.RebuildTreeFunction = [this](FTreeListView& Tree)
        {
            CSkeleton* Skeleton = GetAsset<CSkeleton>();
            TFunction<void(FTreeNodeID, const FSkeletonResource::FBoneInfo&)> AddChildrenRecursive;

            AddChildrenRecursive = [&](FTreeNodeID ParentItem, const FSkeletonResource::FBoneInfo& Bone)
            {
                int32 Index = Skeleton->GetSkeletonResource()->FindBoneIndex(Bone.Name);

                for (int32 Child : Skeleton->GetSkeletonResource()->GetChildBones(Index))
                {
                    const FSkeletonResource::FBoneInfo& ChildBone = Skeleton->GetSkeletonResource()->GetBone(Child);

                    FTreeNodeID NewNode = Tree.CreateNode(ParentItem, FStringView(ChildBone.Name.c_str()));
                    Tree.Get<FTreeNodeState>(NewNode).bExpanded = true;
                    AddChildrenRecursive(NewNode, ChildBone);
                }
            };

            for (int32 BoneIndex : Skeleton->GetSkeletonResource()->GetRootBones())
            {
                const FSkeletonResource::FBoneInfo& Bone = Skeleton->GetSkeletonResource()->GetBone(BoneIndex);
                FTreeNodeID RootItem = Tree.CreateNode(InvalidTreeNode, FStringView(Bone.Name.c_str()));
                Tree.Get<FTreeNodeState>(RootItem).bExpanded = true;

                AddChildrenRecursive(RootItem, Bone);
            }
        };

        BoneListContext.ItemSelectedFunction = [this] (FTreeListView& Tree, FTreeNodeID Item, bool)
        {
            if (!Item.IsValid())
            {
                SelectedBone = NAME_None;
            }
            else
            {
                SelectedBone = Tree.Get<FTreeNodeDisplay>(Item).DisplayName;
            }
        };

        BoneListContext.KeyPressedFunction = [this] (FTreeListView& Tree, FTreeNodeID Item, ImGuiKey Key) -> bool
        {
            return false;
        };
        
        BoneListView.MarkTreeDirty();
    }

    void FSkeletonEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();
        
        CreateFloorPlane();
        
        DirectionalLightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SSkyLightComponent>(DirectionalLightEntity);
        
        CSkeleton* Skeleton = GetAsset<CSkeleton>();
        
        CameraState.Speed = 5.0f;

        if (!Skeleton->PreviewMesh.IsValid())
        {
            return;
        }

        MeshEntity = World->ConstructEntity("MeshEntity");
        SSkeletalMeshComponent& MeshComponent = World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity);
        MeshComponent.SkeletalMesh = Skeleton->PreviewMesh;
        Skeleton->ComputeBindPoseSkinningMatrices(MeshComponent.BoneTransforms);
        MeshComponent.bRenderBonesDirty = true;
        
        STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);
        STransformComponent& EditorTransform = World->GetComponent<STransformComponent>(EditorEntity);

        FQuat Rotation = Math::FindLookAtRotation(MeshTransform.GetWorldLocation()+ FVector3(0.0f, 0.85f, 0.0f), EditorTransform.GetLocation());
        EditorTransform.SetRotation(Rotation);
    }

    void FSkeletonEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);
        
        if (!World.IsValid())
        {
            return;
        }
        
        CSkeleton* Skeleton = GetAsset<CSkeleton>();
        FSkeletonResource* SkeletonResource = Skeleton->GetSkeletonResource();

        const int32 SelectedIndex = (SelectedBone != NAME_None) ? SkeletonResource->FindBoneIndex(SelectedBone) : INDEX_NONE;
        if (SelectedIndex == INDEX_NONE && Skeleton->Sockets.empty())
        {
            return;
        }

        // Bind-pose world transforms; shared by the selected-bone overlay and the socket markers.
        FMatrix4 EntityMatrix = FMatrix4(1.0f);
        if (MeshEntity != entt::null)
        {
            EntityMatrix = World->GetComponent<STransformComponent>(MeshEntity).GetWorldMatrix();
        }

        TVector<FMatrix4> WorldTransforms;
        WorldTransforms.resize(SkeletonResource->GetNumBones());
        for (int i = 0; i < SkeletonResource->GetNumBones(); ++i)
        {
            const FSkeletonResource::FBoneInfo& Bone = SkeletonResource->GetBone(i);
            if (Bone.ParentIndex == INDEX_NONE)
            {
                WorldTransforms[i] = EntityMatrix * Bone.LocalTransform;
            }
            else
            {
                WorldTransforms[i] = WorldTransforms[Bone.ParentIndex] * Bone.LocalTransform;
            }
        }

        if (SelectedIndex != INDEX_NONE)
        {
            DrawBoneHierarchy(World, SkeletonResource, WorldTransforms, SelectedIndex);
        }

        for (const FMeshSocket& Socket : Skeleton->Sockets)
        {
            const int32 BoneIndex = SkeletonResource->FindBoneIndex(Socket.BoneName);
            if (BoneIndex == INDEX_NONE)
            {
                continue;
            }

            const FMatrix4 SocketMatrix = WorldTransforms[BoneIndex] * Socket.RelativeTransform.GetMatrix();
            const FVector3 Position = FVector3(SocketMatrix[3]);
            constexpr float AxisLength = 0.18f;

            World->DrawSphere(Position, 0.045f, FVector4(1.0f, 0.82f, 0.4f, 1.0f), 12, 2.5f, false);
            World->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[0])) * AxisLength, FVector4(1.0f, 0.2f, 0.2f, 1.0f), 3.0f, false);
            World->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[1])) * AxisLength, FVector4(0.2f, 1.0f, 0.2f, 1.0f), 3.0f, false);
            World->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[2])) * AxisLength, FVector4(0.2f, 0.4f, 1.0f, 1.0f), 3.0f, false);
        }
    }

    void FSkeletonEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FSkeletonEditorTool::OnAssetLoadFinished()
    {
    }

    void FSkeletonEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Hierarchy",
            "Bone tree on the left mirrors the import. Click a bone to select; transforms come from the "
            "bind pose. Multi-select extends sockets/constraints across bones.");
        DrawHelpTextRow("Sockets",
            "Sockets are named transforms relative to a bone. Add via the bone context menu. Runtime code "
            "looks them up by name (e.g. 'WeaponR' for an attachment).");
        DrawHelpTextRow("Retargeting",
            "Two skeletons with identical bone names auto-retarget animations between them. Otherwise, "
            "set up a per-bone mapping table on the asset.");
        DrawHelpTextRow("Preview",
            "Drop a skeletal mesh into the viewport to see the skeleton applied. Useful for verifying "
            "socket positions.");
    }

    void FSkeletonEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);
        
        // Gizmo Control Dropdown
        if (ImGui::BeginMenu(LE_ICON_MOVE_RESIZE " Gizmo Control"))
        {
            const char* operations[] = { "Translate", "Rotate", "Scale" };
            static int currentOp = 0;

            if (ImGui::Combo("##", &currentOp, operations, IM_ARRAYSIZE(operations)))
            {
                switch (currentOp)
                {
                case 0: GuizmoOp = ImGuizmo::TRANSLATE; break;
                case 1: GuizmoOp = ImGuizmo::ROTATE;    break;
                case 2: GuizmoOp = ImGuizmo::SCALE;     break;
                }
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LE_ICON_BONE " Skeleton"))
        {
            DrawSkeletonDebugMenuItems();
            ImGui::EndMenu();
        }
    }

    void FSkeletonEditorTool::InitializeDockingLayout(ImGuiID DockspaceID, const ImVec2& DockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(DockspaceID);

        ImGuiID leftID = 0, centerID = 0, rightID = 0;
        ImGui::DockBuilderSplitNode(DockspaceID, ImGuiDir_Right, 0.25f, &rightID, &centerID);
        ImGui::DockBuilderSplitNode(centerID, ImGuiDir_Left, 0.25f, &leftID, &centerID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), centerID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(SkeletonPropertiesName).c_str(), rightID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(BoneHierarchyName).c_str(), leftID);
    }

    void FSkeletonEditorTool::DrawBoneHierarchy(CWorld* DrawWorld, FSkeletonResource* SkeletonResource, const TVector<FMatrix4>& WorldTransforms, int32 BoneIndex)
    {
        if (BoneIndex < 0 || BoneIndex >= SkeletonResource->GetNumBones())
        {
            return;
        }
    
        const FSkeletonResource::FBoneInfo& Bone = SkeletonResource->GetBone(BoneIndex);
        FVector3 BonePosition = FVector3(WorldTransforms[BoneIndex][3]);
    
        DrawWorld->DrawSphere(BonePosition, 0.025f, FColor::Red, 8, 1.0f, false);
    
        if (Bone.ParentIndex != INDEX_NONE)
        {
            FVector3 ParentPosition = FVector3(WorldTransforms[Bone.ParentIndex][3]);
            DrawWorld->DrawLine(ParentPosition, BonePosition, FColor::Green, 10.0f, false);
        }
    
        TVector<int32> Children = SkeletonResource->GetChildBones(BoneIndex);
        for (int32 ChildIndex : Children)
        {
            DrawBoneHierarchy(DrawWorld, SkeletonResource, WorldTransforms, ChildIndex);
        }
    }
}
