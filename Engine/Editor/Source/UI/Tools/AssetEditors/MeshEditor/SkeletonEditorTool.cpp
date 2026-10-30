#include "SkeletonEditorTool.h"
#include "World/ECS/Registry.h"
#include "ImGuiDrawUtils.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Core/Math/Math.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "Renderer/SkeletonResource.h"
#include "Containers/Algorithm.h"
#include "Containers/StringFormat.h"


namespace Lumina
{
    static const char* SkeletonPropertiesName       = "Skeleton Properties";
    static const char* BoneHierarchyName            = "Bone Hierarchy";

    // Sized for a character standing about two meters tall.
    static constexpr float kSocketMarkerRadius      = 0.045f;


    FSkeletonEditorTool::FSkeletonEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    void FSkeletonEditorTool::OnInitialize()
    {
        PropertyContext.Provide(&SkeletonCtx);
        GetPropertyTable()->SetContext(&PropertyContext);

        CreateToolWindow(SkeletonPropertiesName, [&](bool bFocused)
        {
            CSkeleton* Skeleton = GetAsset<CSkeleton>();
            FSkeletonResource* SkeletonResource = Skeleton->GetSkeletonResource();

            // Socket BoneName fields get the bone tree.
            SkeletonCtx.Skeleton = SkeletonResource;

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
                int32 CurrentIndex = (int32)(&Bone - SkeletonResource->Bones.data());
                while (SkeletonResource->Bones[CurrentIndex].ParentIndex != -1)
                {
                    CurrentIndex = SkeletonResource->Bones[CurrentIndex].ParentIndex;
                    Depth++;
                }
                MaxDepth = Math::Max(MaxDepth, Depth);
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
            
            SocketHierarchy.DrawTree();
        });

        SocketHierarchy.Initialize(GetAsset<CSkeleton>(), nullptr, &GetTransactionManager());
        SocketHierarchy.OnSocketsEdited = [this]() { PropertyTable.MarkDirty(); };
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

        RefreshPreviewMesh();
        if (MeshEntity == ECS::NullEntity)
        {
            return;
        }
        
        STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);
        STransformComponent& EditorTransform = World->GetComponent<STransformComponent>(EditorEntity);

        FQuat Rotation = Math::FindLookAtRotation(MeshTransform.GetWorldLocation()+ FVector3(0.0f, 0.85f, 0.0f), EditorTransform.GetLocation());
        EditorTransform.SetRotation(Rotation);
    }

    void FSkeletonEditorTool::RefreshPreviewMesh()
    {
        if (MeshEntity != ECS::NullEntity)
        {
            World->DestroyEntity(MeshEntity);
            MeshEntity = ECS::NullEntity;
        }

        CSkeleton* Skeleton = GetAsset<CSkeleton>();
        PreviewMeshInUse = Skeleton != nullptr ? Skeleton->PreviewMesh.Get() : nullptr;
        if (Skeleton == nullptr || !Skeleton->PreviewMesh.IsValid())
        {
            return;
        }

        MeshEntity = World->ConstructEntity("MeshEntity");
        SSkeletalMeshComponent& MeshComponent = World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity);
        MeshComponent.SetSkeletalMesh(Skeleton->PreviewMesh.Get());
        Skeleton->ComputeBindPoseSkinningMatrices(MeshComponent.BoneTransforms);
        MeshComponent.bRenderBonesDirty = true;
    }

    void FSkeletonEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);
        
        if (!World.IsValid())
        {
            return;
        }

        if (const CSkeleton* Skeleton = GetAsset<CSkeleton>(); Skeleton != nullptr && Skeleton->PreviewMesh.Get() != PreviewMeshInUse.Get())
        {
            RefreshPreviewMesh();
        }
        
        if (World && World->GetRenderer())
        {
            World->GetRenderSettings().bDrawBillboards = false;
        }
        
        CSkeleton* Skeleton = GetAsset<CSkeleton>();
        FSkeletonResource* SkeletonResource = Skeleton->GetSkeletonResource();

        FMatrix4 EntityMatrix = FMatrix4(1.0f);
        if (MeshEntity != ECS::NullEntity)
        {
            EntityMatrix = World->GetComponent<STransformComponent>(MeshEntity).GetWorldMatrix();
        }

        BoneWorldTransforms.resize(SkeletonResource->GetNumBones());
        for (int32 i = 0; i < SkeletonResource->GetNumBones(); ++i)
        {
            const FSkeletonResource::FBoneInfo& Bone = SkeletonResource->GetBone(i);
            const FMatrix4& ParentMatrix = (Bone.ParentIndex == Constants::kIndexNone) ? EntityMatrix : BoneWorldTransforms[Bone.ParentIndex];
            BoneWorldTransforms[i] = ParentMatrix * Bone.LocalTransform;
        }

        SocketHierarchy.SetBoneWorldTransforms(BoneWorldTransforms);

        const int32 SelectedBoneIndex = SocketHierarchy.GetSelectedBoneIndex();
        if (SelectedBoneIndex >= 0 && SelectedBoneIndex < SkeletonResource->GetNumBones())
        {
            DrawBoneHierarchy(World.Get(), SkeletonResource, BoneWorldTransforms, SelectedBoneIndex);
        }

        SocketHierarchy.DrawSocketMarkers(World.Get(), kSocketMarkerRadius);
    }

    void FSkeletonEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        const ImVec2 ViewportOrigin = ViewportScreenMin;
        FAssetEditorTool::DrawViewportOverlayElements(UpdateContext, ViewportTexture, ViewportSize);
        SocketHierarchy.TickViewport(World.Get(), ViewportOrigin, ViewportSize, GuizmoOp, bViewportHovered, ShouldSuppressViewportClickInput(), kSocketMarkerRadius);
    }

    void FSkeletonEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        SocketHierarchy.CloseOpenDragTransaction();
    }

    void FSkeletonEditorTool::OnAssetLoadFinished()
    {
    }

    // Sockets are edited in the property table too, so any edit can rename, move or remove a row.
    void FSkeletonEditorTool::OnPropertyEditFinished(const FPropertyChangedEvent& Event)
    {
        if (CSkeleton* Skeleton = GetAsset<CSkeleton>())
        {
            Skeleton->BumpSocketGeneration();
        }
        SocketHierarchy.Refresh();
    }

    void FSkeletonEditorTool::OnPostUndoRedo()
    {
        FAssetEditorTool::OnPostUndoRedo();
        SocketHierarchy.OnUndoRedo();
    }

    void FSkeletonEditorTool::OnAssetDataChangedExternally()
    {
        FAssetEditorTool::OnAssetDataChangedExternally();
        SocketHierarchy.Refresh();
    }

    void FSkeletonEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Hierarchy",
            "Bone tree on the left mirrors the import. Click a bone to select; transforms come from the "
            "bind pose.");
        DrawHelpTextRow("Sockets",
            "Sockets are named transforms relative to a bone, listed in gold under their bone. Right-click a "
            "bone to add one and a socket to delete it. Runtime code looks them up by name (e.g. 'WeaponR').");
        DrawHelpTextRow("Retargeting",
            "Two skeletons with identical bone names auto-retarget animations between them.");
        DrawHelpTextRow("Preview",
            "Set Preview Mesh in the details to see the skeleton applied. Useful for verifying "
            "socket positions.");
    }

    void FSkeletonEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);
        
        // Gizmo Control Dropdown
        if (ImGui::BeginMenu(LE_ICON_MOVE_RESIZE " Gizmo Control"))
        {
            const char* operations[] = { "Translate", "Rotate", "Scale" };
            int currentOp = (GuizmoOp == ImGuizmo::ROTATE) ? 1 : (GuizmoOp == ImGuizmo::SCALE) ? 2 : 0;

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
    
        DrawWorld->GetDebugInterface()->DrawSphere(BonePosition, 0.025f, FColor::Red, 8, 1.0f, false);
    
        if (Bone.ParentIndex != Constants::kIndexNone)
        {
            FVector3 ParentPosition = FVector3(WorldTransforms[Bone.ParentIndex][3]);
            DrawWorld->GetDebugInterface()->DrawLine(ParentPosition, BonePosition, FColor::Green, 10.0f, false);
        }
    
        TVector<int32> Children = SkeletonResource->GetChildBones(BoneIndex);
        for (int32 ChildIndex : Children)
        {
            DrawBoneHierarchy(DrawWorld, SkeletonResource, WorldTransforms, ChildIndex);
        }
    }
}
