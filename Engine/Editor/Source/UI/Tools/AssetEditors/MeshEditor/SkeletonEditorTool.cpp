#include "SkeletonEditorTool.h"
#include "World/World.h"
#include "World/ECS/Registry.h"
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
#include "Log/Log.h"


namespace Lumina
{
    static const char* SkeletonPropertiesName       = "Skeleton Properties";
    static const char* BoneHierarchyName            = "Bone Hierarchy";

    // Sized for a character standing about two meters tall.
    static constexpr float kSocketMarkerRadius      = 0.045f;


    FSkeletonEditorTool::FSkeletonEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, *InAsset->GetName(), InAsset, NewObject<CWorld>())
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
            ImGui::Text("Skeleton Name: %s", *SkeletonResource->Name.ToString());
            
            ImGui::Spacing();
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Statistics");
            ImGuiX::Font::PopFont();
            
            FBoneDepthStatistics Stats = ComputeBoneDepthStatistics(*SkeletonResource);

            ImGui::Text("Root Bones: %i", Stats.NumRootBones);
            ImGui::Text("Maximum Hierarchy Depth: %i", Stats.MaxDepth);
            ImGui::Text("Average Bone Depth: %.2f", Stats.AverageDepth);

            if (!Stats.bConsistentHierarchy)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Broken hierarchy: a bone's ParentIndex is cyclic, self-parented or out of range");

                if (!bLoggedInconsistentHierarchy)
                {
                    LOG_WARN("Skeleton '{}' has a broken ParentIndex chain (cycle, self-parent, or out-of-range parent); depth statistics are unavailable", SkeletonResource->Name);
                    bLoggedInconsistentHierarchy = true;
                }
            }
            
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
        World->GetRenderSettings().bDrawBillboards = false;

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

        CSkeleton* Skeleton = GetAsset<CSkeleton>();
        FSkeletonResource* SkeletonResource = Skeleton->GetSkeletonResource();

        FMatrix4 EntityMatrix = FMatrix4(1.0f);
        if (MeshEntity != ECS::NullEntity)
        {
            EntityMatrix = World->GetComponent<STransformComponent>(MeshEntity).GetWorldMatrix();
        }

        BoneWorldTransforms.SetNum(SkeletonResource->GetNumBones());
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
        
        if (ImGui::BeginMenu(LE_ICON_MOVE_RESIZE " Gizmo Control"))
        {
            DrawGizmoOperationCombo(GuizmoOp);
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

        ImGui::DockBuilderDockWindow(*GetToolWindowName(ViewportWindowName), centerID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(SkeletonPropertiesName), rightID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(BoneHierarchyName), leftID);
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

    FSkeletonEditorTool::FBoneDepthStatistics FSkeletonEditorTool::ComputeBoneDepthStatistics(const FSkeletonResource& SkeletonResource)
    {
        FBoneDepthStatistics Stats;
        Stats.bConsistentHierarchy = SkeletonResource.HasConsistentHierarchy();

        for (int32 BoneIndex = 0; BoneIndex < SkeletonResource.GetNumBones(); ++BoneIndex)
        {
            if (SkeletonResource.Bones[BoneIndex].ParentIndex == Constants::kIndexNone)
            {
                Stats.NumRootBones++;
            }
        }

        if (!Stats.bConsistentHierarchy)
        {
            return Stats;
        }

        // Safe as a single forward pass only because HasConsistentHierarchy just proved ParentIndex < BoneIndex for every bone.
        TVector<int32> Depths;
        Depths.SetNum(SkeletonResource.GetNumBones());
        int32 TotalDepth = 0;

        for (int32 BoneIndex = 0; BoneIndex < SkeletonResource.GetNumBones(); ++BoneIndex)
        {
            const int32 ParentIndex = SkeletonResource.Bones[BoneIndex].ParentIndex;
            Depths[BoneIndex] = ParentIndex == Constants::kIndexNone ? 0 : Depths[ParentIndex] + 1;
            Stats.MaxDepth = Math::Max(Stats.MaxDepth, Depths[BoneIndex]);
            TotalDepth += Depths[BoneIndex];
        }

        Stats.AverageDepth = SkeletonResource.GetNumBones() > 0 ? (float)TotalDepth / SkeletonResource.GetNumBones() : 0.0f;
        return Stats;
    }
}
