#include "PhysicsAssetEditorTool.h"
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Core/Object/Package/Package.h"
#include "Physics/PhysicsScene.h"
#include "Physics/Ray/RayCast.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyTable.h"
#include "UI/Tools/Transactions/EditorTransaction.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/World.h"
#include "imgui.h"
#include "implot.h"
#include "Renderer/SkeletonResource.h"
#include "Tools/UI/ImGui/EditorColors.h"

namespace Lumina
{
    static const char* SkeletonTreeWindowName   = "Skeleton Tree";
    static const char* ConstraintsWindowName    = "Constraints";
    static const char* PhysicsAssetDetailsWindowName        = "Details";

    static const FVector4 BodyColor             = FVector4(0.35f, 0.70f, 1.00f, 1.0f);
    static const FVector4 BodySelectedColor     = FVector4(1.00f, 0.75f, 0.15f, 1.0f);
    static const FVector4 BoneLineColor         = FVector4(0.35f, 0.35f, 0.40f, 1.0f);

    constexpr float SelectedConeLength = 0.18f;
    constexpr float ConeLength = 0.12f;
    constexpr float HardTwistArcShare = 0.5f;
    constexpr float SoftTwistArcShare = 0.4f;

    static const char* ShapeIcon(ERagdollBodyShape Shape)
    {
        switch (Shape)
        {
        case ERagdollBodyShape::Box:    return LE_ICON_CUBE_OUTLINE;
        case ERagdollBodyShape::Sphere: return LE_ICON_CIRCLE_OUTLINE;
        default:                        return LE_ICON_BOWLING;
        }
    }

    // Euler degrees for the shortest rotation taking From onto To; both must be unit length.
    static FVector3 EulerDegreesFromTo(const FVector3& From, const FVector3& To)
    {
        const float D = Math::Clamp(Math::Dot(From, To), -1.0f, 1.0f);

        FQuat Rotation = FQuat::Identity();
        if (D < -0.9999f)
        {
            FVector3 Axis = Math::Cross(From, FVector3(1.0f, 0.0f, 0.0f));
            if (Math::LengthSquared(Axis) < 1e-6f)
            {
                Axis = Math::Cross(From, FVector3(0.0f, 0.0f, 1.0f));
            }
            Rotation = Math::FromAxisAngle(Math::Normalize(Axis), Math::Pi<float>());
        }
        else if (D < 0.9999f)
        {
            Rotation = Math::FromAxisAngle(Math::Normalize(Math::Cross(From, To)), Math::Acos(D));
        }

        return Math::Degrees(Math::EulerAngles(Rotation));
    }

    // Where the limb points when swung onto the elliptical cone's rim at Phi, in the joint frame, the inverse of ClampToSoftRange's reach.
    static FVector3 SwingRimDirection(float Swing1, float Swing2, float Phi)
    {
        const FVector3 RotationVector(Swing1 * Math::Cos(Phi), Swing2 * Math::Sin(Phi), 0.0f);
        const float Angle = Math::Length(RotationVector);
        return Angle > 1e-6f ? Math::AngleAxis(Angle, RotationVector / Angle) * FVector3(0.0f, 0.0f, 1.0f) : FVector3(0.0f, 0.0f, 1.0f);
    }

    static void DrawSwingCone(IPrimitiveDrawInterface& Draw, const FVector3& Pivot, const FQuat& Frame, float Swing1, float Swing2, float Length,
                              const FVector4& Color, float Thickness)
    {
        constexpr int32 Segments = 32;
        constexpr int32 SegmentsPerSpoke = 8;

        FVector3 Previous = Pivot + Frame * SwingRimDirection(Swing1, Swing2, 0.0f) * Length;
        for (int32 Index = 1; Index <= Segments; ++Index)
        {
            const FVector3 Current = Pivot + Frame * SwingRimDirection(Swing1, Swing2, Math::TwoPi<float>() * (float)Index / (float)Segments) * Length;
            Draw.DrawLine(Previous, Current, Color, Thickness, false);
            if (Index % SegmentsPerSpoke == 0)
            {
                Draw.DrawLine(Pivot, Current, Color, Thickness, false);
            }
            Previous = Current;
        }
    }

    static FVector4 DimmedColor(const FVector4& Color, bool bSelected)
    {
        return bSelected ? Color : FVector4(FVector3(Color) * 0.45f, Color.w);
    }

    static void GatherChildOffsets(const FSkeletonResource& Resource, int32 BoneIndex, TVector<FVector3>& OutOffsets)
    {
        OutOffsets.Reset();
        for (int32 ChildIndex : Resource.GetChildBones(BoneIndex))
        {
            OutOffsets.Emplace(Resource.GetBone(ChildIndex).LocalTransform[3]);
        }
    }

    // False means the bone is too small to be worth simulating and Body was left untouched.
    static bool ShapeBodyForBone(SPhysicsBodySetup& Body, const TVector<FVector3>& ChildOffsets, float MinBoneLength)
    {
        if (ChildOffsets.IsEmpty())
        {
            return false;
        }

        if (ChildOffsets.Num() == 1)
        {
            const FVector3& Offset = ChildOffsets[0];
            const float BoneLength = Math::Length(Offset);
            if (BoneLength < MinBoneLength || BoneLength < 1e-4f)
            {
                return false;
            }

            Body.Shape = ERagdollBodyShape::Capsule;
            Body.Radius = Math::Clamp(BoneLength * 0.25f, 0.02f, 0.12f);
            Body.HalfHeight = Math::Max(0.0f, BoneLength * 0.5f - Body.Radius);
            Body.HalfExtent = FVector3(Body.Radius, BoneLength * 0.5f, Body.Radius);
            Body.TranslationOffset = Offset * 0.5f;
            Body.RotationOffset = EulerDegreesFromTo(FVector3(0.0f, 1.0f, 0.0f), Math::Normalize(Offset));
            return true;
        }

        // Averaging child offsets cancels out on a branching bone, leaving it pinned to its bind pose.
        float MeanDistance = 0.0f;
        for (const FVector3& Offset : ChildOffsets)
        {
            MeanDistance += Math::Length(Offset);
        }
        MeanDistance /= (float)ChildOffsets.Num();

        if (MeanDistance < MinBoneLength || MeanDistance < 1e-4f)
        {
            return false;
        }

        Body.Shape = ERagdollBodyShape::Sphere;
        Body.Radius = Math::Clamp(MeanDistance * 0.5f, 0.03f, 0.15f);
        Body.HalfExtent = FVector3(Body.Radius);
        Body.TranslationOffset = FVector3(0.0f);
        Body.RotationOffset = FVector3(0.0f);
        return true;
    }

    static FTreeNodeID FindTreeNodeForBone(FTreeListView& Tree, FTreeNodeID Parent, int32 BoneIndex)
    {
        const int32 ChildCount = Tree.NumChildNodes(Parent);
        for (int32 i = 0; i < ChildCount; ++i)
        {
            const FTreeNodeID Child = Tree.GetChildNode(Parent, i);
            if (Tree.Get<int32>(Child) == BoneIndex)
            {
                return Child;
            }

            const FTreeNodeID Found = FindTreeNodeForBone(Tree, Child, BoneIndex);
            if (Found.IsValid())
            {
                return Found;
            }
        }

        return InvalidTreeNode;
    }

    FPhysicsAssetEditorTool::FPhysicsAssetEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, *InAsset->GetName(), InAsset, NewObject<CWorld>())
    {
    }

    CSkeleton* FPhysicsAssetEditorTool::GetSkeleton()
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        return PhysicsAsset != nullptr ? PhysicsAsset->Skeleton.Get() : nullptr;
    }

    FSkeletonResource* FPhysicsAssetEditorTool::GetSkeletonResource()
    {
        CSkeleton* Skeleton = GetSkeleton();
        return Skeleton != nullptr ? Skeleton->GetSkeletonResource() : nullptr;
    }

    void FPhysicsAssetEditorTool::OnInitialize()
    {
        DetailsTable = MakeUnique<FPropertyTable>();
        DetailsTable->SetPostEditCallback([this](const FPropertyChangedEvent&)
        {
            NotifyAssetDataChanged();
            RetuneSimulatedRagdoll();
            RebuildBodySubtreeMask();
            BoneListView.MarkTreeDirty();
        });
        WirePropertyTableUndo(*DetailsTable);

        SetupTreeContext();

        CreateToolWindow(SkeletonTreeWindowName, [this](bool) { DrawSkeletonTreeWindow(); });
        CreateToolWindow(ConstraintsWindowName, [this](bool) { DrawConstraintsWindow(); });
        CreateToolWindow(PhysicsAssetDetailsWindowName, [this](bool) { DrawDetailsWindow(); });

        CachedSkeleton = GetSkeleton();
        RebuildBodySubtreeMask();
        BoneListView.MarkTreeDirty();
    }

    void FPhysicsAssetEditorTool::SetupTreeContext()
    {
        BoneListContext.RebuildTreeFunction = [this](FTreeListView& Tree)
        {
            FSkeletonResource* Resource = GetSkeletonResource();
            if (Resource == nullptr)
            {
                return;
            }

            CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

            TFunction<void(FTreeNodeID, int32)> AddBone;
            AddBone = [&](FTreeNodeID ParentNode, int32 BoneIndex)
            {
                const FName BoneName = Resource->GetBone(BoneIndex).Name;
                const int32 BodyIndex = PhysicsAsset->FindBodyIndex(BoneName);
                const char* Icon = (BodyIndex != Constants::kIndexNone) ? ShapeIcon(PhysicsAsset->Bodies[BodyIndex].Shape) : LE_ICON_BONE;

                // The widget draws IconText OVER the row, so without the prefix it lands on the name.
                const FString Label = FString(Icon) + "  " + *BoneName;

                FTreeNodeID Node = Tree.CreateNode(ParentNode, FStringView(*Label));
                Tree.EmplaceUserData<int32>(Node, BoneIndex);
                Tree.Get<FTreeNodeState>(Node).bExpanded = true;

                {
                    FTreeNodeDisplay& Display = Tree.Get<FTreeNodeDisplay>(Node);
                    Display.IconText = Icon;
                    if (BodyIndex != Constants::kIndexNone)
                    {
                        Display.IconColor = ImVec4(0.35f, 0.70f, 1.0f, 1.0f);
                        Display.DisplayColor = ImVec4(0.88f, 0.94f, 1.0f, 1.0f);
                    }
                    else
                    {
                        Display.IconColor = ImVec4(0.45f, 0.45f, 0.48f, 1.0f);
                        Display.DisplayColor = ImVec4(0.60f, 0.60f, 0.62f, 1.0f);
                    }
                }

                for (int32 ChildIndex : Resource->GetChildBones(BoneIndex))
                {
                    AddBone(Node, ChildIndex);
                }
            };

            for (int32 RootIndex : Resource->GetRootBones())
            {
                AddBone(InvalidTreeNode, RootIndex);
            }
        };

        BoneListContext.FilterFunction = [this](FTreeListView& Tree, FTreeNodeID Item) -> bool
        {
            if (!bBodyBonesOnly)
            {
                return true;
            }

            const int32 BoneIndex = Tree.Get<int32>(Item);
            return BoneIndex >= 0 && BoneIndex < BodySubtreeMask.Num() && BodySubtreeMask[BoneIndex] != 0;
        };

        BoneListContext.ItemSelectedFunction = [this](FTreeListView& Tree, FTreeNodeID Item, bool)
        {
            if (!Item.IsValid())
            {
                ClearSelection();
                return;
            }

            const int32 BoneIndex = Tree.Get<int32>(Item);
            FSkeletonResource* Resource = GetSkeletonResource();
            if (Resource == nullptr || BoneIndex < 0 || BoneIndex >= Resource->GetNumBones())
            {
                return;
            }

            SelectedBone = Resource->GetBone(BoneIndex).Name;

            // A bone with no body empties the details panel rather than keeping a stale one on screen.
            SelectBody(GetAsset<CPhysicsAsset>()->FindBodyIndex(SelectedBone));
        };

        BoneListContext.ItemContextMenuFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            if (!Item.IsValid())
            {
                return;
            }

            FSkeletonResource* Resource = GetSkeletonResource();
            const int32 BoneIndex = Tree.Get<int32>(Item);
            if (Resource == nullptr || BoneIndex < 0 || BoneIndex >= Resource->GetNumBones())
            {
                return;
            }

            const FName BoneName = Resource->GetBone(BoneIndex).Name;
            CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
            const int32 BodyIndex = PhysicsAsset->FindBodyIndex(BoneName);

            // Restructuring under a live ragdoll leaves simulated bodies describing a setup that is gone.
            ImGui::BeginDisabled(bSimulating);

            if (BodyIndex == Constants::kIndexNone)
            {
                if (ImGui::MenuItem(LE_ICON_PLUS " Add Body"))
                {
                    SelectBody(AddBodyForBone(BoneName));
                }
            }
            else
            {
                if (ImGui::MenuItem(LE_ICON_LINK " Constrain To Parent Body"))
                {
                    SelectConstraint(AddConstraintForBone(BoneName));
                }

                ImGui::Separator();
                if (ImGui::MenuItem(LE_ICON_DELETE " Remove Body"))
                {
                    RemoveBodyAt(BodyIndex);
                }
            }

            ImGui::EndDisabled();
        };

        BoneListContext.KeyPressedFunction = [this](FTreeListView&, FTreeNodeID, ImGuiKey Key) -> bool
        {
            if (Key == ImGuiKey_Delete && !bSimulating && SelectionMode == EPhysicsAssetSelection::Body)
            {
                RemoveBodyAt(SelectedBodyIndex);
                return true;
            }
            return false;
        };
    }

    void FPhysicsAssetEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();
        World->GetRenderSettings().bDrawBillboards = false;

        CreateFloorPlane();

        LightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(LightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(LightEntity);
        World->EmplaceComponent<SSkyLightComponent>(LightEntity);

        CameraState.Speed = 5.0f;

        RefreshForSkeletonChange();
    }

    void FPhysicsAssetEditorTool::RefreshForSkeletonChange()
    {
        // The live ragdoll is bound to the mesh entity this is about to destroy.
        StopSimulation();

        CachedSkeleton = GetSkeleton();
        CachedPreviewMesh = CachedSkeleton.IsValid() ? CachedSkeleton->PreviewMesh.Get() : nullptr;

        ClearSelection();
        SelectedBone = NAME_None;
        RebuildBodySubtreeMask();
        BoneListView.MarkTreeDirty();

        if (!World.IsValid())
        {
            return;
        }

        if (MeshEntity != ECS::NullEntity)
        {
            World->DestroyEntity(MeshEntity);
            MeshEntity = ECS::NullEntity;
        }

        CSkeleton* Skeleton = GetSkeleton();
        if (Skeleton == nullptr || !Skeleton->PreviewMesh.IsValid())
        {
            return;
        }

        MeshEntity = World->ConstructEntity("PreviewMesh");
        SSkeletalMeshComponent& MeshComponent = World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity);
        MeshComponent.SetSkeletalMesh(Skeleton->PreviewMesh.Get());
        Skeleton->ComputeBindPoseSkinningMatrices(MeshComponent.BoneTransforms);
        MeshComponent.bRenderBonesDirty = true;

        STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);
        STransformComponent& EditorTransform = World->GetComponent<STransformComponent>(EditorEntity);

        const FQuat LookAt = Math::FindLookAtRotation(MeshTransform.GetWorldLocation() + FVector3(0.0f, 0.85f, 0.0f), EditorTransform.GetLocation());
        EditorTransform.SetRotation(LookAt);
    }

    void FPhysicsAssetEditorTool::OnAssetDataChangedExternally()
    {
        FAssetEditorTool::OnAssetDataChangedExternally();
        RefreshForSkeletonChange();
    }

    void FPhysicsAssetEditorTool::OnPropertyEditFinished(const FPropertyChangedEvent& Event)
    {
        RebuildBodySubtreeMask();
        BoneListView.MarkTreeDirty();
    }

    void FPhysicsAssetEditorTool::OnPostUndoRedo()
    {
        FAssetEditorTool::OnPostUndoRedo();

        // A restore rewrites Bodies and Constraints wholesale, so pointers and the selection go stale.
        DetailsTarget = nullptr;
        DetailsType = nullptr;

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        if (SelectedBodyIndex >= PhysicsAsset->Bodies.Num()
            || SelectedConstraintIndex >= PhysicsAsset->Constraints.Num())
        {
            ClearSelection();
        }

        SyncDetailsTable();
        RebuildBodySubtreeMask();
        BoneListView.MarkTreeDirty();
        RetuneSimulatedRagdoll();
    }

    void FPhysicsAssetEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        if (!World.IsValid())
        {
            return;
        }

        // Skeleton is an editable property, so it can be swapped from the details panel at any time.
        const CSkeleton* Skeleton = GetSkeleton();
        if (CachedSkeleton.Get() != Skeleton || (Skeleton != nullptr && CachedPreviewMesh.Get() != Skeleton->PreviewMesh.Get()))
        {
            RefreshForSkeletonChange();
        }

        SyncDetailsTable();

        // The overlays are drawn from the bind pose, which the live ragdoll has already left behind.
        if (bSimulating)
        {
            DrawSimulationOverlay();
            return;
        }

        RebuildBoneWorldTransforms();

        if (BoneWorldTransforms.IsEmpty())
        {
            return;
        }

        if (bDrawBoneLines)
        {
            FSkeletonResource* Resource = GetSkeletonResource();
            for (int32 i = 0; i < Resource->GetNumBones(); ++i)
            {
                const int32 ParentIndex = Resource->GetBone(i).ParentIndex;
                if (ParentIndex != Constants::kIndexNone)
                {
                    World->GetDebugInterface()->DrawLine(FVector3(BoneWorldTransforms[ParentIndex][3]), FVector3(BoneWorldTransforms[i][3]), BoneLineColor, 1.0f, false);
                }
            }
        }

        if (bDrawBodies)
        {
            DrawBodies();
        }

        if (bDrawConstraints)
        {
            DrawConstraints();
        }
    }

    void FPhysicsAssetEditorTool::RebuildBoneWorldTransforms()
    {
        BoneWorldTransforms.Reset();

        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr || Resource->GetNumBones() == 0)
        {
            return;
        }

        FMatrix4 EntityMatrix = FMatrix4(1.0f);
        if (MeshEntity != ECS::NullEntity)
        {
            EntityMatrix = World->GetComponent<STransformComponent>(MeshEntity).GetWorldMatrix();
        }

        BoneWorldTransforms.SetNum(Resource->GetNumBones());
        for (int32 i = 0; i < Resource->GetNumBones(); ++i)
        {
            const FSkeletonResource::FBoneInfo& Bone = Resource->GetBone(i);
            if (Bone.ParentIndex == Constants::kIndexNone)
            {
                BoneWorldTransforms[i] = EntityMatrix * Bone.LocalTransform;
            }
            else
            {
                BoneWorldTransforms[i] = BoneWorldTransforms[Bone.ParentIndex] * Bone.LocalTransform;
            }
        }
    }

    void FPhysicsAssetEditorTool::RebuildBodySubtreeMask()
    {
        BodySubtreeMask.Reset();

        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr)
        {
            return;
        }

        BodySubtreeMask.SetNum(Resource->GetNumBones(), 0);

        for (const SPhysicsBodySetup& Body : GetAsset<CPhysicsAsset>()->Bodies)
        {
            int32 Index = Resource->FindBoneIndex(Body.BoneName);
            while (Index >= 0 && Index < BodySubtreeMask.Num())
            {
                BodySubtreeMask[Index] = 1;
                Index = Resource->GetBone(Index).ParentIndex;
            }
        }
    }

    FMatrix4 FPhysicsAssetEditorTool::GetBodyWorldMatrix(const SPhysicsBodySetup& Body)
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr)
        {
            return FMatrix4(1.0f);
        }

        const int32 BoneIndex = Resource->FindBoneIndex(Body.BoneName);
        if (BoneIndex == Constants::kIndexNone || BoneIndex >= BoneWorldTransforms.Num())
        {
            return FMatrix4(1.0f);
        }

        const FMatrix4 Offset = Math::Translate(FMatrix4(1.0f), Body.TranslationOffset) * Math::ToMatrix4(FQuat(Math::Radians(Body.RotationOffset)));
        return BoneWorldTransforms[BoneIndex] * Offset;
    }

    bool FPhysicsAssetEditorTool::HasBodyBone(const SPhysicsBodySetup& Body)
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr)
        {
            return false;
        }
        const int32 BoneIndex = Resource->FindBoneIndex(Body.BoneName);
        return BoneIndex != Constants::kIndexNone && BoneIndex < BoneWorldTransforms.Num();
    }

    void FPhysicsAssetEditorTool::DrawBodies()
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        for (int32 i = 0; i < PhysicsAsset->Bodies.Num(); ++i)
        {
            const SPhysicsBodySetup& Body = PhysicsAsset->Bodies[i];
            if (!HasBodyBone(Body))
            {
                continue;
            }
            const bool bSelected = (SelectionMode == EPhysicsAssetSelection::Body && i == SelectedBodyIndex);
            const FVector4 Color = bSelected ? BodySelectedColor : BodyColor;
            const float Thickness = bSelected ? 6.0f : 3.5f;

            const FMatrix4 BodyMatrix = GetBodyWorldMatrix(Body);
            const FVector3 Center = FVector3(BodyMatrix[3]);

            switch (Body.Shape)
            {
            case ERagdollBodyShape::Capsule:
                {
                    const FVector3 Axis = Math::Normalize(FVector3(BodyMatrix[1]));
                    World->GetDebugInterface()->DrawCapsule(Center - Axis * Body.HalfHeight, Center + Axis * Body.HalfHeight, Body.Radius, Color, 16, Thickness, false);
                }
                break;

            case ERagdollBodyShape::Sphere:
                World->GetDebugInterface()->DrawSphere(Center, Body.Radius, Color, 16, Thickness, false);
                break;

            case ERagdollBodyShape::Box:
                World->GetDebugInterface()->DrawBox(Center, Body.HalfExtent, Math::ToQuat(BodyMatrix), Color, Thickness, false);
                break;
            }

            // Authoring RotationOffset needs the frame visible, but drawing every body is unreadable.
            if (bSelected)
            {
                constexpr float AxisLength = 0.12f;
                World->GetDebugInterface()->DrawLine(Center, Center + Math::Normalize(FVector3(BodyMatrix[0])) * AxisLength, FVector4(1.0f, 0.2f, 0.2f, 1.0f), 2.5f, false);
                World->GetDebugInterface()->DrawLine(Center, Center + Math::Normalize(FVector3(BodyMatrix[1])) * AxisLength, FVector4(0.2f, 1.0f, 0.2f, 1.0f), 2.5f, false);
                World->GetDebugInterface()->DrawLine(Center, Center + Math::Normalize(FVector3(BodyMatrix[2])) * AxisLength, FVector4(0.2f, 0.4f, 1.0f, 1.0f), 2.5f, false);
            }
        }
    }

    bool FPhysicsAssetEditorTool::GetConstraintFrame(const SPhysicsConstraintSetup& Constraint, FVector3& OutPivot, FQuat& OutFrame)
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        const int32 ChildIndex = Resource != nullptr ? Resource->FindBoneIndex(Constraint.ChildBone) : Constants::kIndexNone;
        if (ChildIndex == Constants::kIndexNone || ChildIndex >= BoneWorldTransforms.Num())
        {
            return false;
        }

        OutPivot = FVector3(BoneWorldTransforms[ChildIndex][3]);

        FVector3 TowardChildren(0.0f);
        for (int32 GrandChild : Resource->GetChildBones(ChildIndex))
        {
            TowardChildren += FVector3(BoneWorldTransforms[GrandChild][3]) - OutPivot;
        }

        const int32 ParentIndex = Resource->FindBoneIndex(Constraint.ParentBone);
        const FVector3 AwayFromParent = ParentIndex != Constants::kIndexNone && ParentIndex < BoneWorldTransforms.Num()
            ? OutPivot - FVector3(BoneWorldTransforms[ParentIndex][3]) : FVector3(0.0f);

        OutFrame = MakeRagdollJointFrame(TowardChildren, AwayFromParent, FVector3(BoneWorldTransforms[ChildIndex][1]));
        return true;
    }

    SPhysicsConstraintSetup* FPhysicsAssetEditorTool::GetSelectedConstraint()
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        const bool bValid = SelectionMode == EPhysicsAssetSelection::Constraint
            && SelectedConstraintIndex >= 0 && SelectedConstraintIndex < PhysicsAsset->Constraints.Num();
        return bValid ? &PhysicsAsset->Constraints[SelectedConstraintIndex] : nullptr;
    }

    float FPhysicsAssetEditorTool::GetPreviewScale()
    {
        return MeshEntity != ECS::NullEntity ? World->GetComponent<STransformComponent>(MeshEntity).GetWorldMaxScaleCached() : 1.0f;
    }

    void FPhysicsAssetEditorTool::DrawConstraints()
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr)
        {
            return;
        }

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        IPrimitiveDrawInterface& Draw = *World->GetDebugInterface();
        const CEditorColorSettings& Palette = EditorColors::Palette();
        const float Scale = GetPreviewScale();

        for (int32 i = 0; i < PhysicsAsset->Constraints.Num(); ++i)
        {
            const SPhysicsConstraintSetup& Constraint = PhysicsAsset->Constraints[i];

            FVector3 Pivot;
            FQuat Frame;
            if (!GetConstraintFrame(Constraint, Pivot, Frame))
            {
                continue;
            }

            const bool bSelected = (SelectionMode == EPhysicsAssetSelection::Constraint && i == SelectedConstraintIndex);
            const FVector4 HardColor = DimmedColor(Palette.Danger, bSelected);
            const FVector4 SoftColor = DimmedColor(Palette.Warning, bSelected);
            const float Thickness = bSelected ? 3.0f : 1.5f;
            const float Length = (bSelected ? SelectedConeLength : ConeLength) * Scale;

            const int32 ParentIndex = Resource->FindBoneIndex(Constraint.ParentBone);
            if (ParentIndex != Constants::kIndexNone && ParentIndex < BoneWorldTransforms.Num())
            {
                Draw.DrawLine(Pivot, FVector3(BoneWorldTransforms[ParentIndex][3]), DimmedColor(Palette.Accent, bSelected), Thickness, false);
            }

            const FVector3 Limits = Constraint.GetLimitsRadians();
            const float SoftStart = Constraint.GetSoftLimitStart();
            DrawSwingCone(Draw, Pivot, Frame, Limits.y, Limits.z, Length, HardColor, Thickness);

            // Arcs about the limb, zero along the frame's X axis where DrawArc starts.
            const FVector3 TwistAxis = Frame * FVector3(0.0f, 0.0f, 1.0f);
            const FVector3 ArcCenter = Pivot + TwistAxis * Length;
            Draw.DrawArc(ArcCenter, TwistAxis, Length * HardTwistArcShare, -Limits.x, 2.0f * Limits.x, HardColor, 24, Thickness, false);

            if (Constraint.bSoftLimits)
            {
                DrawSwingCone(Draw, Pivot, Frame, Limits.y * SoftStart, Limits.z * SoftStart, Length, SoftColor, Thickness);
                Draw.DrawArc(ArcCenter, TwistAxis, Length * SoftTwistArcShare, -Limits.x * SoftStart, 2.0f * Limits.x * SoftStart, SoftColor, 24, Thickness, false);
            }
        }
    }

    int32 FPhysicsAssetEditorTool::PickBody(const FVector3& RayOrigin, const FVector3& RayDirection)
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        int32 BestBody = Constants::kIndexNone;
        float BestDistance = 1e30f;

        for (int32 i = 0; i < PhysicsAsset->Bodies.Num(); ++i)
        {
            const SPhysicsBodySetup& Body = PhysicsAsset->Bodies[i];
            if (!HasBodyBone(Body))
            {
                continue;
            }
            const FMatrix4 BodyMatrix = GetBodyWorldMatrix(Body);
            const FVector3 Center = FVector3(BodyMatrix[3]);

            float Distance = 0.0f;
            bool bHit = false;

            switch (Body.Shape)
            {
            case ERagdollBodyShape::Capsule:
                {
                    const FVector3 Axis = Math::Normalize(FVector3(BodyMatrix[1]));
                    bHit = ShapeHandles::RayHitsCapsule(RayOrigin, RayDirection, Center - Axis * Body.HalfHeight,
                                          Center + Axis * Body.HalfHeight, Body.Radius, Distance);
                }
                break;

            case ERagdollBodyShape::Sphere:
                bHit = ShapeHandles::RayHitsSphere(RayOrigin, RayDirection, Center, Body.Radius, Distance);
                break;

            case ERagdollBodyShape::Box:
                bHit = ShapeHandles::RayHitsBox(RayOrigin, RayDirection, BodyMatrix, Body.HalfExtent, Distance);
                break;
            }

            if (bHit && Distance < BestDistance)
            {
                BestDistance = Distance;
                BestBody = i;
            }
        }

        return BestBody;
    }

    void FPhysicsAssetEditorTool::GatherShapeHandles(TVector<FShapeHandle>& OutHandles)
    {
        OutHandles.Reset();

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        FVector3 Pivot;
        FQuat Frame;
        SPhysicsConstraintSetup* SelectedConstraint = GetSelectedConstraint();
        if (SelectedConstraint != nullptr && GetConstraintFrame(*SelectedConstraint, Pivot, Frame))
        {
            const SPhysicsConstraintSetup& Constraint = *SelectedConstraint;
            const FVector3 Limits = Constraint.GetLimitsRadians();
            const float SoftStart = Constraint.GetSoftLimitStart();
            const float Length = SelectedConeLength * GetPreviewScale();
            const FVector3 AxisX = Frame * FVector3(1.0f, 0.0f, 0.0f);
            const FVector3 AxisY = Frame * FVector3(0.0f, 1.0f, 0.0f);
            const FVector3 AxisZ = Frame * FVector3(0.0f, 0.0f, 1.0f);
            const FVector3 ArcCenter = Pivot + AxisZ * Length;
            auto OnTwistArc = [&](float Angle, float RadiusShare)
            {
                return ArcCenter + (AxisX * Math::Cos(Angle) + AxisY * Math::Sin(Angle)) * (Length * RadiusShare);
            };

            // Soft dots sit opposite the hard ones, so the two never overlap.
            OutHandles.Emplace(EShapeHandle::SwingLimit1, Pivot + Frame * SwingRimDirection(Limits.y, Limits.z, 0.0f) * Length, AxisX, Pivot);
            OutHandles.Emplace(EShapeHandle::SwingLimit2, Pivot + Frame * SwingRimDirection(Limits.y, Limits.z, Math::HalfPi<float>()) * Length, AxisY, Pivot);
            OutHandles.Emplace(EShapeHandle::TwistLimit, OnTwistArc(Limits.x, HardTwistArcShare), AxisZ, ArcCenter);
            if (Constraint.bSoftLimits)
            {
                OutHandles.Emplace(EShapeHandle::SoftSwing, Pivot + Frame * SwingRimDirection(Limits.y * SoftStart, Limits.z * SoftStart, Math::Pi<float>()) * Length, AxisX, Pivot);
                OutHandles.Emplace(EShapeHandle::SoftTwist, OnTwistArc(-Limits.x * SoftStart, SoftTwistArcShare), AxisZ, ArcCenter);
            }
            return;
        }

        if (SelectionMode != EPhysicsAssetSelection::Body
            || SelectedBodyIndex < 0 || SelectedBodyIndex >= PhysicsAsset->Bodies.Num())
        {
            return;
        }

        const SPhysicsBodySetup& Body = PhysicsAsset->Bodies[SelectedBodyIndex];
        const FMatrix4 BodyMatrix = GetBodyWorldMatrix(Body);

        const FVector3 Center = FVector3(BodyMatrix[3]);
        const FVector3 AxisX = Math::Normalize(FVector3(BodyMatrix[0]));
        const FVector3 AxisY = Math::Normalize(FVector3(BodyMatrix[1]));
        const FVector3 AxisZ = Math::Normalize(FVector3(BodyMatrix[2]));

        switch (Body.Shape)
        {
        case ERagdollBodyShape::Capsule:
            OutHandles.Emplace(EShapeHandle::Radius,     Center + AxisX * Body.Radius,     AxisX, Center);
            OutHandles.Emplace(EShapeHandle::HalfHeight, Center + AxisY * Body.HalfHeight, AxisY, Center);
            break;

        case ERagdollBodyShape::Sphere:
            OutHandles.Emplace(EShapeHandle::Radius, Center + AxisX * Body.Radius, AxisX, Center);
            break;

        case ERagdollBodyShape::Box:
            OutHandles.Emplace(EShapeHandle::ExtentX, Center + AxisX * Body.HalfExtent.x, AxisX, Center);
            OutHandles.Emplace(EShapeHandle::ExtentY, Center + AxisY * Body.HalfExtent.y, AxisY, Center);
            OutHandles.Emplace(EShapeHandle::ExtentZ, Center + AxisZ * Body.HalfExtent.z, AxisZ, Center);
            break;
        }
    }

    void FPhysicsAssetEditorTool::ApplyShapeHandleDrag(const FShapeHandle& Handle, const FVector3& RayOrigin, const FVector3& RayDirection)
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        FVector3 Pivot;
        FQuat Frame;
        SPhysicsConstraintSetup* SelectedConstraint = GetSelectedConstraint();
        if (SelectedConstraint != nullptr && GetConstraintFrame(*SelectedConstraint, Pivot, Frame))
        {
            // Twist dots sweep around the limb from the frame's X axis, swing dots tip the limb away from Z about their own axis.
            const bool bTwist = Handle.Type == EShapeHandle::TwistLimit || Handle.Type == EShapeHandle::SoftTwist;
            const FVector3 Zero = Frame * (bTwist ? FVector3(1.0f, 0.0f, 0.0f) : FVector3(0.0f, 0.0f, 1.0f));
            const FVector3 Ninety = Frame * (Handle.Type == EShapeHandle::SwingLimit2 ? FVector3(1.0f, 0.0f, 0.0f) : FVector3(0.0f, 1.0f, 0.0f));

            float Angle = 0.0f;
            if (!ShapeHandles::AngleOnPlaneToRay(Handle.Anchor, Zero, Ninety, RayOrigin, RayDirection, Angle))
            {
                return;
            }

            SPhysicsConstraintSetup& Constraint = *SelectedConstraint;
            const float Degrees = Math::Min(Math::Degrees(Math::Abs(Angle)), 180.0f);
            switch (Handle.Type)
            {
            case EShapeHandle::SwingLimit1: Constraint.Swing1LimitDegrees = Degrees; break;
            case EShapeHandle::SwingLimit2: Constraint.Swing2LimitDegrees = Degrees; break;
            case EShapeHandle::TwistLimit:  Constraint.TwistLimitDegrees = Degrees; break;
            case EShapeHandle::SoftSwing:   Constraint.SoftLimitStart = Math::Clamp(Degrees / Math::Max(Constraint.Swing1LimitDegrees, 0.1f), 0.0f, 1.0f); break;
            case EShapeHandle::SoftTwist:   Constraint.SoftLimitStart = Math::Clamp(Degrees / Math::Max(Constraint.TwistLimitDegrees, 0.1f), 0.0f, 1.0f); break;
            default: return;
            }

            NotifyAssetDataChanged();
            return;
        }

        if (SelectedBodyIndex < 0 || SelectedBodyIndex >= PhysicsAsset->Bodies.Num())
        {
            return;
        }

        float AxisDistance = 0.0f;
        if (!ShapeHandles::ClosestParamOnAxisToRay(Handle.Anchor, Handle.Axis, RayOrigin, RayDirection, AxisDistance))
        {
            return;
        }

        SPhysicsBodySetup& Body = PhysicsAsset->Bodies[SelectedBodyIndex];

        // Radii and extents are clamped away from zero; a half-height of zero is a legitimate sphere-capsule.
        const float Positive = Math::Max(AxisDistance, 0.001f);

        switch (Handle.Type)
        {
        case EShapeHandle::Radius:     Body.Radius = Positive; break;
        case EShapeHandle::HalfHeight: Body.HalfHeight = Math::Max(AxisDistance, 0.0f); break;
        case EShapeHandle::ExtentX:    Body.HalfExtent.x = Positive; break;
        case EShapeHandle::ExtentY:    Body.HalfExtent.y = Positive; break;
        case EShapeHandle::ExtentZ:    Body.HalfExtent.z = Positive; break;
        default: return;
        }

        // Rows read through to the struct, so rebuilding the tree every drag frame is pure cost.
        NotifyAssetDataChanged();
    }

    void FPhysicsAssetEditorTool::CloseOpenDragTransaction()
    {
        ShapeHandles::CloseOpenDragTransaction(*this, HandleState);
    }

    void FPhysicsAssetEditorTool::ApplyGizmoMatrix(const FMatrix4& NewBodyMatrix)
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        if (Resource == nullptr || SelectedBodyIndex < 0 || SelectedBodyIndex >= PhysicsAsset->Bodies.Num())
        {
            return;
        }

        SPhysicsBodySetup& Body = PhysicsAsset->Bodies[SelectedBodyIndex];

        const int32 BoneIndex = Resource->FindBoneIndex(Body.BoneName);
        if (BoneIndex == Constants::kIndexNone || BoneIndex >= BoneWorldTransforms.Num())
        {
            return;
        }

        // The gizmo hands back a world frame; the asset stores the frame relative to its bone.
        const FMatrix4 LocalOffset = Math::Inverse(BoneWorldTransforms[BoneIndex]) * NewBodyMatrix;

        Body.TranslationOffset = FVector3(LocalOffset[3]);

        // Normalized because a bind pose carrying scale would otherwise leak it into the quaternion.
        Body.RotationOffset = Math::Degrees(Math::EulerAngles(Math::Normalize(Math::ToQuat(LocalOffset))));

        NotifyAssetDataChanged();
    }

    void FPhysicsAssetEditorTool::SyncTreeSelectionToBody(int32 BodyIndex)
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        if (Resource == nullptr || BodyIndex < 0 || BodyIndex >= PhysicsAsset->Bodies.Num())
        {
            return;
        }

        SelectedBone = PhysicsAsset->Bodies[BodyIndex].BoneName;

        const int32 BoneIndex = Resource->FindBoneIndex(SelectedBone);
        if (BoneIndex == Constants::kIndexNone || BoneListView.IsDirty())
        {
            return;
        }

        const FTreeNodeID Node = FindTreeNodeForBone(BoneListView, InvalidTreeNode, BoneIndex);
        if (Node.IsValid())
        {
            BoneListView.SetSelectionSilent(Node);
            BoneListView.RequestScrollToNode(Node);
        }
    }

    void FPhysicsAssetEditorTool::UpdateSimulationGrab(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize)
    {
        constexpr float GrabReach = 100.0f;
        constexpr float GrabStiffness = 400.0f;
        constexpr float GrabDamping = 30.0f;

        Physics::IPhysicsScene* Scene = World->GetPhysicsScene();
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

        const ImVec2 MousePos = ImGui::GetMousePos();
        FVector3 RayOrigin, RayDirection;
        if (!ShapeHandles::BuildViewportRay(World.Get(), ViewportOrigin, ViewportSize, MousePos, RayOrigin, RayDirection))
        {
            return;
        }

        if (!GrabbedTarget.IsSet())
        {
            if (!bViewportHovered || !ImGui::GetIO().KeyShift || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                return;
            }

            SRayCastSettings Settings;
            Settings.Start = RayOrigin;
            Settings.End = RayOrigin + RayDirection * GrabReach;

            const TOptional<SRayResult> Hit = Scene->CastRay(Settings);

            // The floor is static, so grabbing it would latch the drag onto something that can never move.
            if (!Hit.IsSet() || (ECS::FEntity)Hit->Entity == FloorBodyEntity)
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
            GrabLocalOffset = Math::Inverse(BodyState.Rotation) * (Hit->Location - BodyState.Position);
            return;
        }

        Physics::FPhysicsBodyState BodyState;
        if (!Scene->TryGetTargetState(GrabbedTarget, BodyState) || BodyState.Mass <= 0.0f)
        {
            GrabbedTarget = {};
            return;
        }
        const FVector3 Target = RayOrigin + RayDirection * GrabDistance;
        const FVector3 AttachPoint = BodyState.Position + BodyState.Rotation * GrabLocalOffset;

        // The damping term is what stops the body oscillating once the cursor stops.
        const FVector3 Force = (Target - AttachPoint) * GrabStiffness
                             - (BodyState.LinearVelocity + Math::Cross(BodyState.AngularVelocity, AttachPoint - BodyState.CenterOfMass)) * GrabDamping;

        Scene->AddForceAtTarget(GrabbedTarget, Force, AttachPoint);

        World->GetDebugInterface()->DrawLine(AttachPoint, Target, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 2.5f, false);
        World->GetDebugInterface()->DrawSphere(Target, 0.035f, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 10, 2.0f, false);
    }

    FMatrix4 FPhysicsAssetEditorTool::GetGizmoMatrix()
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        return GetBodyWorldMatrix(PhysicsAsset->Bodies[SelectedBodyIndex]);
    }

    void FPhysicsAssetEditorTool::PickShapeAtRay(const FVector3& RayOrigin, const FVector3& RayDirection)
    {
        const int32 Picked = PickBody(RayOrigin, RayDirection);
        SelectBody(Picked);
        if (Picked != Constants::kIndexNone)
        {
            SyncTreeSelectionToBody(Picked);
        }
    }

    void FPhysicsAssetEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        const ImVec2 ViewportOrigin = ViewportScreenMin;

        FAssetEditorTool::DrawViewportOverlayElements(UpdateContext, ViewportTexture, ViewportSize);

        SCameraComponent* Camera = World.IsValid() ? World->GetCamera()->GetActiveCamera() : nullptr;
        if (Camera == nullptr)
        {
            HandleState.ActiveHandle = EShapeHandle::None;
            return;
        }

        if (bSimulating)
        {
            // Gated on viewport focus, so Esc typed into another panel does not kill the run.
            if ((bViewportFocused || bViewportHovered) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                StopSimulation();
                return;
            }

            HandleState.ActiveHandle = EShapeHandle::None;
            CloseOpenDragTransaction();
            UpdateSimulationGrab(ViewportOrigin, ViewportSize);
            return;
        }

        if (BoneWorldTransforms.IsEmpty())
        {
            return;
        }

        if (bViewportHovered && ImGui::IsKeyPressed(ImGuiKey_Space))
        {
            BodyGizmoOp = (BodyGizmoOp == ImGuizmo::TRANSLATE) ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE;
        }

        FMatrix4 Projection = Camera->GetProjectionMatrix();
        Projection[1][1] *= -1.0f;

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        FShapeHandleFrame Frame;
        Frame.World            = World.Get();
        Frame.ViewMatrix       = Camera->GetViewMatrix();
        Frame.Projection       = Projection;
        Frame.ViewportOrigin   = ViewportOrigin;
        Frame.ViewportSize     = ViewportSize;
        Frame.GizmoOp          = BodyGizmoOp;
        Frame.bViewportHovered = bViewportHovered;
        Frame.bSuppressClicks  = ShouldSuppressViewportClickInput();
        Frame.bHasGizmoTarget  = SelectionMode == EPhysicsAssetSelection::Body
                              && SelectedBodyIndex >= 0 && SelectedBodyIndex < PhysicsAsset->Bodies.Num();

        TickShapeHandles(*this, HandleState, Frame);
    }

    int32 FPhysicsAssetEditorTool::AddBodyForBone(const FName& BoneName)
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        const int32 Existing = PhysicsAsset->FindBodyIndex(BoneName);
        if (Existing != Constants::kIndexNone)
        {
            return Existing;
        }

        BeginAssetTransaction("Add Body");

        SPhysicsBodySetup& Body = PhysicsAsset->Bodies.EmplaceGetRef();
        Body.BoneName = BoneName;

        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource != nullptr)
        {
            const int32 BoneIndex = Resource->FindBoneIndex(BoneName);
            if (BoneIndex != Constants::kIndexNone)
            {
                // No minimum here, since an explicit add always produces a body even on a stub bone.
                TVector<FVector3> ChildOffsets;
                GatherChildOffsets(*Resource, BoneIndex, ChildOffsets);
                ShapeBodyForBone(Body, ChildOffsets, 0.0f);
            }
        }

        NotifyAssetDataChanged();
        RebuildBodySubtreeMask();
        BoneListView.MarkTreeDirty();
        EndAssetTransaction();

        return PhysicsAsset->Bodies.Num() - 1;
    }

    void FPhysicsAssetEditorTool::RemoveBodyAt(int32 BodyIndex)
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        if (BodyIndex < 0 || BodyIndex >= PhysicsAsset->Bodies.Num())
        {
            return;
        }

        BeginAssetTransaction("Remove Body");

        const FName BoneName = PhysicsAsset->Bodies[BodyIndex].BoneName;
        PhysicsAsset->Bodies.RemoveAt(BodyIndex);

        // A joint referencing a body that no longer exists cannot be built at runtime.
        for (int32 i = PhysicsAsset->Constraints.Num() - 1; i >= 0; --i)
        {
            if (PhysicsAsset->Constraints[i].ChildBone == BoneName || PhysicsAsset->Constraints[i].ParentBone == BoneName)
            {
                PhysicsAsset->Constraints.RemoveAt(i);
            }
        }

        ClearSelection();
        NotifyAssetDataChanged();
        RebuildBodySubtreeMask();
        BoneListView.MarkTreeDirty();
        EndAssetTransaction();
    }

    void FPhysicsAssetEditorTool::ClearAllBodies()
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        if (PhysicsAsset->Bodies.IsEmpty() && PhysicsAsset->Constraints.IsEmpty())
        {
            return;
        }

        BeginAssetTransaction("Clear Bodies");

        PhysicsAsset->Bodies.Reset();
        PhysicsAsset->Constraints.Reset();

        ClearSelection();
        NotifyAssetDataChanged();
        RebuildBodySubtreeMask();
        BoneListView.MarkTreeDirty();
        EndAssetTransaction();
    }

    int32 FPhysicsAssetEditorTool::AddConstraintForBone(const FName& ChildBone)
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr)
        {
            return Constants::kIndexNone;
        }

        const int32 BoneIndex = Resource->FindBoneIndex(ChildBone);
        if (BoneIndex == Constants::kIndexNone)
        {
            return Constants::kIndexNone;
        }

        const FName ParentBone = FindAncestorBodyBone(Resource->GetBone(BoneIndex).ParentIndex);
        if (ParentBone == NAME_None)
        {
            return Constants::kIndexNone;
        }

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        for (int32 i = 0; i < PhysicsAsset->Constraints.Num(); ++i)
        {
            if (PhysicsAsset->Constraints[i].ChildBone == ChildBone)
            {
                return i;
            }
        }

        BeginAssetTransaction("Add Constraint");

        SPhysicsConstraintSetup& Constraint = PhysicsAsset->Constraints.EmplaceGetRef();
        Constraint.ChildBone = ChildBone;
        Constraint.ParentBone = ParentBone;

        NotifyAssetDataChanged();
        EndAssetTransaction();

        return PhysicsAsset->Constraints.Num() - 1;
    }

    void FPhysicsAssetEditorTool::RemoveConstraintAt(int32 ConstraintIndex)
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        if (ConstraintIndex < 0 || ConstraintIndex >= PhysicsAsset->Constraints.Num())
        {
            return;
        }

        BeginAssetTransaction("Remove Constraint");

        PhysicsAsset->Constraints.RemoveAt(ConstraintIndex);

        ClearSelection();
        NotifyAssetDataChanged();
        EndAssetTransaction();
    }

    FName FPhysicsAssetEditorTool::FindAncestorBodyBone(int32 BoneIndex)
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr)
        {
            return NAME_None;
        }

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        int32 Current = BoneIndex;
        while (Current >= 0 && Current < Resource->GetNumBones())
        {
            const FName BoneName = Resource->GetBone(Current).Name;
            if (PhysicsAsset->FindBodyIndex(BoneName) != Constants::kIndexNone)
            {
                return BoneName;
            }
            Current = Resource->GetBone(Current).ParentIndex;
        }

        return NAME_None;
    }

    void FPhysicsAssetEditorTool::GenerateFromSkeleton(bool bReplaceExisting)
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr)
        {
            return;
        }

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        BeginAssetTransaction(bReplaceExisting ? "Generate Bodies" : "Fill Body Gaps");

        if (bReplaceExisting)
        {
            PhysicsAsset->Bodies.Reset();
            PhysicsAsset->Constraints.Reset();
            ClearSelection();
        }

        for (int32 i = 0; i < Resource->GetNumBones(); ++i)
        {
            const FName BoneName = Resource->GetBone(i).Name;
            if (PhysicsAsset->FindBodyIndex(BoneName) != Constants::kIndexNone)
            {
                continue;
            }

            // A leaf bone has no length to fit a shape to, and a body per one buries the ragdoll in stubs.
            TVector<FVector3> ChildOffsets;
            GatherChildOffsets(*Resource, i, ChildOffsets);

            SPhysicsBodySetup Candidate;
            Candidate.BoneName = BoneName;
            if (!ShapeBodyForBone(Candidate, ChildOffsets, GenerateMinBoneLength))
            {
                continue;
            }

            PhysicsAsset->Bodies.Add(Candidate);
        }

        // Every body exists by the second pass, so the ancestor walk sees the whole set.
        for (int32 i = 0; i < PhysicsAsset->Bodies.Num(); ++i)
        {
            const FName ChildBone = PhysicsAsset->Bodies[i].BoneName;

            const int32 BoneIndex = Resource->FindBoneIndex(ChildBone);
            if (BoneIndex == Constants::kIndexNone)
            {
                continue;
            }

            const FName ParentBone = FindAncestorBodyBone(Resource->GetBone(BoneIndex).ParentIndex);
            if (ParentBone == NAME_None)
            {
                continue;
            }

            bool bAlreadyConstrained = false;
            for (const SPhysicsConstraintSetup& Existing : PhysicsAsset->Constraints)
            {
                if (Existing.ChildBone == ChildBone)
                {
                    bAlreadyConstrained = true;
                    break;
                }
            }

            if (bAlreadyConstrained)
            {
                continue;
            }

            SPhysicsConstraintSetup& Constraint = PhysicsAsset->Constraints.EmplaceGetRef();
            Constraint.ChildBone = ChildBone;
            Constraint.ParentBone = ParentBone;
        }

        NotifyAssetDataChanged();
        RebuildBodySubtreeMask();
        BoneListView.MarkTreeDirty();
        EndAssetTransaction();
    }

    void FPhysicsAssetEditorTool::SelectBody(int32 BodyIndex)
    {
        SelectedConstraintIndex = Constants::kIndexNone;
        SelectedBodyIndex = BodyIndex;
        SelectionMode = (BodyIndex != Constants::kIndexNone) ? EPhysicsAssetSelection::Body : EPhysicsAssetSelection::None;
    }

    void FPhysicsAssetEditorTool::SelectConstraint(int32 ConstraintIndex)
    {
        SelectedBodyIndex = Constants::kIndexNone;
        SelectedConstraintIndex = ConstraintIndex;
        SelectionMode = (ConstraintIndex != Constants::kIndexNone) ? EPhysicsAssetSelection::Constraint : EPhysicsAssetSelection::None;
    }

    void FPhysicsAssetEditorTool::ClearSelection()
    {
        SelectionMode = EPhysicsAssetSelection::None;
        SelectedBodyIndex = Constants::kIndexNone;
        SelectedConstraintIndex = Constants::kIndexNone;
    }

    void FPhysicsAssetEditorTool::DrawSimulationOverlay()
    {
        ++SimulationFrames;

        if (FloorBodyEntity != ECS::NullEntity)
        {
            const STransformComponent& FloorTransform = World->GetComponent<STransformComponent>(FloorBodyEntity);
            if (const SBoxColliderComponent* FloorBox = World->TryGetComponent<SBoxColliderComponent>(FloorBodyEntity))
            {
                World->GetDebugInterface()->DrawBox(FloorTransform.GetLocation(), FloorBox->HalfExtent, FloorTransform.GetRotation(),
                    FVector4(0.25f, 1.0f, 0.45f, 1.0f), 2.0f, false);
            }

            // The body is built from a deferred queue, so report what landed rather than what was asked.
            const SRigidBodyComponent* FloorBody = World->TryGetComponent<SRigidBodyComponent>(FloorBodyEntity);
            if (!bFloorBodyReported && FloorBody != nullptr && World->GetPhysicsScene()->GetBodyStatus(FloorBodyEntity) == Physics::EPhysicsBodyStatus::Ready)
            {
                bFloorBodyReported = true;
                LOG_INFO("PhysicsAsset sim: floor body {} created at {}, half extent {}", FloorBodyEntity.Value,
                    FloorTransform.GetLocation().y, World->GetComponent<SBoxColliderComponent>(FloorBodyEntity).HalfExtent.y);
            }
            else if (!bFloorBodyReported && SimulationFrames == 60)
            {
                LOG_WARN("PhysicsAsset sim: floor body was never created; the ragdoll has nothing to land on.");
            }
        }

        // Recovered from the skinning matrices, since Global is Skin times the inverse of InvBind.
        FSkeletonResource* Resource = GetSkeletonResource();
        const SSkeletalMeshComponent* MeshComponent = (MeshEntity != ECS::NullEntity)
            ? World->TryGetComponent<SSkeletalMeshComponent>(MeshEntity) : nullptr;

        if (Resource == nullptr || MeshComponent == nullptr
            || MeshComponent->BoneTransforms.Num() != Resource->GetNumBones())
        {
            return;
        }

        const FMatrix4 EntityMatrix = World->GetComponent<STransformComponent>(MeshEntity).GetWorldMatrix();

        for (int32 i = 0; i < Resource->GetNumBones(); ++i)
        {
            const FMatrix4 BoneWorld = EntityMatrix * (MeshComponent->BoneTransforms[i] * Math::Inverse(Resource->GetBone(i).InvBindMatrix));
            const FVector3 Position = FVector3(BoneWorld[3]);

            const bool bHasBody = GetAsset<CPhysicsAsset>()->FindBodyIndex(Resource->GetBone(i).Name) != Constants::kIndexNone;
            World->GetDebugInterface()->DrawSphere(Position, bHasBody ? 0.03f : 0.015f,
                bHasBody ? BodyColor : FVector4(0.5f, 0.5f, 0.55f, 1.0f), 8, 2.0f, false);

            const int32 ParentIndex = Resource->GetBone(i).ParentIndex;
            if (ParentIndex != Constants::kIndexNone)
            {
                const FMatrix4 ParentWorld = EntityMatrix * (MeshComponent->BoneTransforms[ParentIndex] * Math::Inverse(Resource->GetBone(ParentIndex).InvBindMatrix));
                World->GetDebugInterface()->DrawLine(FVector3(ParentWorld[3]), Position, BoneLineColor, 1.5f, false);
            }
        }
    }

    void FPhysicsAssetEditorTool::StartSimulation()
    {
        if (bSimulating || !World.IsValid() || MeshEntity == ECS::NullEntity)
        {
            return;
        }

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        if (PhysicsAsset->Bodies.IsEmpty())
        {
            return;
        }

        World->EnsurePhysicsScene();

        // The visual floor plane carries no collider, so the ragdoll needs something to land on.
        FloorBodyEntity = World->ConstructEntity("Simulation Floor");
        World->GetComponent<STransformComponent>(FloorBodyEntity).SetLocation(FVector3(0.0f, -0.5f, 0.0f));

        // The body is built from what the component holds at construction, so pass values in.
        SRigidBodyComponent BodyDesc;
        BodyDesc.BodyType = EBodyType::Static;
        BodyDesc.CollisionProfile.Layer = ECollisionProfiles::Static;
        BodyDesc.CollisionProfile.Mask = ECollisionProfiles::Static | ECollisionProfiles::Dynamic;
        World->EmplaceComponent<SRigidBodyComponent>(FloorBodyEntity, BodyDesc);

        SBoxColliderComponent BoxDesc;
        BoxDesc.HalfExtent = FVector3(25.0f, 0.5f, 25.0f);
        World->EmplaceComponent<SBoxColliderComponent>(FloorBodyEntity, BoxDesc);

        SRagdollComponent& Ragdoll = World->EmplaceComponent<SRagdollComponent>(MeshEntity);
        Ragdoll.PhysicsAsset = PhysicsAsset;
        Ragdoll.State = ERagdollState::Simulated;

        // Following the root would drag the preview out of frame, so leave the entity where it is.
        Ragdoll.bDriveEntityFromRoot = false;

        // Editor worlds are created paused, which stops systems and the physics step alike.
        World->SetPaused(false);

        SimulationFrames = 0;
        bFloorBodyReported = false;
        bSimulating = true;
    }

    void FPhysicsAssetEditorTool::StopSimulation()
    {
        if (!bSimulating || !World.IsValid())
        {
            return;
        }

        World->SetPaused(true);
        GrabbedTarget = {};

        if (MeshEntity != ECS::NullEntity)
        {
            World->RemoveComponent<SRagdollComponent>(MeshEntity);

            // Back to the bind pose the bodies were authored against.
            if (CSkeleton* Skeleton = GetSkeleton())
            {
                SSkeletalMeshComponent& MeshComponent = World->GetComponent<SSkeletalMeshComponent>(MeshEntity);
                Skeleton->ComputeBindPoseSkinningMatrices(MeshComponent.BoneTransforms);
                MeshComponent.bRenderBonesDirty = true;
            }

            World->GetComponent<STransformComponent>(MeshEntity).SetLocation(FVector3(0.0f));
        }

        if (FloorBodyEntity != ECS::NullEntity)
        {
            World->DestroyEntity(FloorBodyEntity);
            FloorBodyEntity = ECS::NullEntity;
        }

        bSimulating = false;
    }

    void FPhysicsAssetEditorTool::RetuneSimulatedRagdoll()
    {
        Physics::IPhysicsScene* Scene = bSimulating ? World->GetPhysicsScene() : nullptr;
        if (Scene != nullptr)
        {
            Scene->RetuneRagdollLimits(MeshEntity, *GetAsset<CPhysicsAsset>());
        }
    }

    float FPhysicsAssetEditorTool::SampleSoftLimitPull(const SPhysicsConstraintSetup& Constraint, int32 Axis, float AngleRadians)
    {
        const FVector3 Axes[] = { FVector3(0.0f, 0.0f, 1.0f), FVector3(1.0f, 0.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f) };
        FQuat Held = Math::AngleAxis(AngleRadians, Axes[Axis]);
        const float Depth = ClampToSoftRange(Held, Constraint.GetLimitsRadians(), Constraint.GetSoftLimitStart());

        // The ligament springs toward the held pose at the limit stiffness scaled by depth, retuned to the inertia it swings.
        const float Overshoot = AngleRadians - 2.0f * Math::Acos(Math::Min(Math::Abs(Held.w), 1.0f));
        const float Omega = Math::TwoPi<float>() * Constraint.LimitStiffness * Depth;
        return Omega * Omega * Overshoot;
    }

    void FPhysicsAssetEditorTool::CopySoftLimits(CPhysicsAsset& Asset, int32 SourceIndex, const FSkeletonResource* SubtreeSkeleton)
    {
        if (SourceIndex < 0 || SourceIndex >= Asset.Constraints.Num())
        {
            return;
        }

        const SPhysicsConstraintSetup Source = Asset.Constraints[SourceIndex];
        const int32 SubtreeRoot = SubtreeSkeleton != nullptr ? SubtreeSkeleton->FindBoneIndex(Source.ChildBone) : Constants::kIndexNone;
        for (SPhysicsConstraintSetup& Target : Asset.Constraints)
        {
            if (SubtreeSkeleton != nullptr)
            {
                int32 Bone = SubtreeSkeleton->FindBoneIndex(Target.ChildBone);
                while (SubtreeSkeleton->IsBoneIndexValid(Bone) && Bone != SubtreeRoot)
                {
                    Bone = SubtreeSkeleton->GetBone(Bone).ParentIndex;
                }
                if (Bone != SubtreeRoot || SubtreeRoot == Constants::kIndexNone)
                {
                    continue;
                }
            }

            Target.bSoftLimits       = Source.bSoftLimits;
            Target.SoftLimitStart    = Source.SoftLimitStart;
            Target.LimitStiffness    = Source.LimitStiffness;
            Target.LimitDampingRatio = Source.LimitDampingRatio;
            Target.MaxLimitTorque    = Source.MaxLimitTorque;
        }
    }

    void FPhysicsAssetEditorTool::ApplySelectedSoftLimits(bool bSubtreeOnly)
    {
        BeginAssetTransaction(bSubtreeOnly ? "Apply Soft Limits To Subtree" : "Apply Soft Limits To All");
        CopySoftLimits(*GetAsset<CPhysicsAsset>(), SelectedConstraintIndex, bSubtreeOnly ? GetSkeletonResource() : nullptr);
        NotifyAssetDataChanged();
        EndAssetTransaction();
        RetuneSimulatedRagdoll();
    }

    void FPhysicsAssetEditorTool::DrawSoftLimitResponse(const SPhysicsConstraintSetup& Constraint)
    {
        constexpr int32 Samples = 64;
        constexpr const char* AxisNames[] = { "Twist", "Swing 1", "Swing 2" };

        ImGui::SeparatorText("Soft Limit Response");
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            if (Axis > 0)
            {
                ImGui::SameLine();
            }
            ImGui::RadioButton(AxisNames[Axis], &ResponseCurveAxis, Axis);
        }

        const float HardDegrees = Math::Degrees(Constraint.GetLimitsRadians()[ResponseCurveAxis]);
        double Angles[Samples];
        double Pulls[Samples];
        for (int32 i = 0; i < Samples; ++i)
        {
            const float Degrees = HardDegrees * (float)i / (float)(Samples - 1);
            Angles[i] = Degrees;
            Pulls[i] = SampleSoftLimitPull(Constraint, ResponseCurveAxis, Math::Radians(Degrees));
        }

        const double SoftStart = HardDegrees * Constraint.GetSoftLimitStart();
        if (ImPlot::BeginPlot("##SoftLimitResponse", ImVec2(-1.0f, 150.0f), ImPlotFlags_CanvasOnly))
        {
            ImPlot::SetupAxes("Angle (deg)", "Pull (rad/s^2)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetNextLineStyle(EditorColors::Accent(), 2.0f);
            ImPlot::PlotLine("Pull", Angles, Pulls, Samples);
            ImPlot::SetNextLineStyle(EditorColors::Warning());
            ImPlot::PlotInfLines("Soft Start", &SoftStart, 1);
            ImPlot::SetNextLineStyle(EditorColors::Danger());
            ImPlot::PlotInfLines("Hard Limit", &Angles[Samples - 1], 1);
            ImPlot::TagX(SoftStart, EditorColors::Warning(), "soft");
            ImPlot::TagX(Angles[Samples - 1], EditorColors::Danger(), "hard");
            ImPlot::EndPlot();
        }
        ImGuiX::TextTooltip("Angular pull the ligament gives the limb, per unit of the inertia it swings, up to its Max Limit Torque.");

        if (ImGui::Button(LE_ICON_CONTENT_COPY " Apply To All"))
        {
            ApplySelectedSoftLimits(false);
        }
        ImGuiX::TextTooltip("Copy this joint's soft limit settings onto every constraint.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_SITEMAP " Apply To Subtree"))
        {
            ApplySelectedSoftLimits(true);
        }
        ImGuiX::TextTooltip("Copy this joint's soft limit settings onto the constraints at and below its child bone.");
    }

    void FPhysicsAssetEditorTool::SyncDetailsTable()
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        void* Target = nullptr;
        CStruct* Type = nullptr;

        if (SelectionMode == EPhysicsAssetSelection::Body
            && SelectedBodyIndex >= 0 && SelectedBodyIndex < PhysicsAsset->Bodies.Num())
        {
            Target = &PhysicsAsset->Bodies[SelectedBodyIndex];
            Type = SPhysicsBodySetup::StaticStruct();
        }
        else if (SPhysicsConstraintSetup* Constraint = GetSelectedConstraint())
        {
            Target = Constraint;
            Type = SPhysicsConstraintSetup::StaticStruct();
        }

        // Comparing the address catches a reallocation, since rows cache a pointer into the element.
        if (Target != DetailsTarget || Type != DetailsType)
        {
            DetailsTarget = Target;
            DetailsType = Type;

            if (Target != nullptr)
            {
                DetailsTable->SetObject(Target, Type);
                DetailsTable->MarkDirty();
            }
        }
    }

    void FPhysicsAssetEditorTool::DrawSkeletonTreeWindow()
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        if (GetSkeletonResource() == nullptr)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), LE_ICON_ALERT " No skeleton assigned.");
            ImGui::TextWrapped("Assign a Skeleton in the Details panel. Bodies are authored by bone name, so the asset needs one before anything can be added.");
            return;
        }

        if (bSimulating)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.25f, 0.20f, 1.0f));
            if (ImGui::Button(LE_ICON_STOP " Stop"))
            {
                StopSimulation();
            }
            ImGui::PopStyleColor();
            ImGuiX::TextTooltip("Stop simulating and restore the bind pose. (Esc)");
        }
        else
        {
            ImGui::BeginDisabled(PhysicsAsset->Bodies.IsEmpty() || MeshEntity == ECS::NullEntity);
            if (ImGui::Button(LE_ICON_PLAY " Simulate"))
            {
                StartSimulation();
            }
            ImGui::EndDisabled();

            if (MeshEntity == ECS::NullEntity)
            {
                ImGuiX::TextTooltip("The Skeleton needs a Preview Mesh before the ragdoll can be simulated.");
            }
            else
            {
                ImGuiX::TextTooltip("Drop the ragdoll into the preview world and run physics on it.");
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // Editing the body list under a live ragdoll leaves simulated bodies describing a setup that is gone.
        ImGui::BeginDisabled(bSimulating);

        if (ImGui::Button(LE_ICON_AUTO_FIX " Generate"))
        {
            GenerateFromSkeleton(true);
        }
        ImGuiX::TextTooltip("Replace all bodies and constraints with a capsule per bone and a swing-twist joint per pair.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_PLUS " Fill Gaps"))
        {
            GenerateFromSkeleton(false);
        }
        ImGuiX::TextTooltip("Add bodies for bones that do not have one yet, leaving existing bodies untouched.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_DELETE " Clear"))
        {
            ClearAllBodies();
        }

        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat("Min Bone Length", &GenerateMinBoneLength, 0.005f, 0.0f, 1.0f, "%.3f m");
        ImGuiX::TextTooltip("Bones shorter than this are skipped when generating.");

        ImGui::EndDisabled();

        bool bOnlyBodies = bBodyBonesOnly;
        if (ImGui::Checkbox("Bodies Only", &bOnlyBodies))
        {
            bBodyBonesOnly = bOnlyBodies;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("%d bodies, %d constraints", PhysicsAsset->Bodies.Num(), PhysicsAsset->Constraints.Num());

        ImGui::Separator();
        ImGui::Spacing();

        BoneListView.Draw(BoneListContext);
    }

    void FPhysicsAssetEditorTool::DrawConstraintsWindow()
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        if (PhysicsAsset->Constraints.IsEmpty())
        {
            ImGui::TextDisabled("No constraints.");
            ImGui::TextWrapped("Right-click a bone that has a body and choose \"Constrain To Parent Body\".");
            return;
        }

        if (ImGui::BeginTable("##Constraints", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Parent", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Child", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##Actions", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.x * 2.0f);
            ImGui::TableHeadersRow();

            int32 PendingRemoval = Constants::kIndexNone;

            for (int32 i = 0; i < PhysicsAsset->Constraints.Num(); ++i)
            {
                const SPhysicsConstraintSetup& Constraint = PhysicsAsset->Constraints[i];

                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                const bool bSelected = (SelectionMode == EPhysicsAssetSelection::Constraint && i == SelectedConstraintIndex);

                // A labeled row-spanning selectable draws across the columns and swallows the delete clicks.
                if (ImGui::Selectable("##Row", bSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    SelectConstraint(i);
                }

                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextUnformatted(*Constraint.ParentBone);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(*Constraint.ChildBone);

                ImGui::TableNextColumn();
                ImGui::BeginDisabled(bSimulating);
                if (ImGui::SmallButton(LE_ICON_DELETE))
                {
                    PendingRemoval = i;
                }
                ImGui::EndDisabled();

                ImGui::PopID();
            }

            ImGui::EndTable();

            // Deferred, since erasing mid-iteration invalidates the loop and every row after it.
            if (PendingRemoval != Constants::kIndexNone)
            {
                RemoveConstraintAt(PendingRemoval);
            }
        }
    }

    void FPhysicsAssetEditorTool::DrawDetailsWindow()
    {
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::SeparatorText(SelectionMode == EPhysicsAssetSelection::Constraint ? "Constraint" : "Body");
        ImGuiX::Font::PopFont();
        ImGui::Spacing();

        // Constraint limits stay editable while simulating, since the running ragdoll is retuned on each edit.
        const bool bEditingConstraint = SelectionMode == EPhysicsAssetSelection::Constraint;
        if (bSimulating)
        {
            ImGui::TextDisabled(bEditingConstraint ? "Limit edits retune the running ragdoll." : "Stop simulating to edit.");
        }

        int32 Orphaned = 0;
        for (const SPhysicsBodySetup& Body : GetAsset<CPhysicsAsset>()->Bodies)
        {
            Orphaned += HasBodyBone(Body) ? 0 : 1;
        }
        if (Orphaned > 0)
        {
            ImGui::TextColored(EditorColors::Warning(), LE_ICON_ALERT " %d body(s) name a bone this skeleton does not have.", Orphaned);
        }
        ImGui::BeginDisabled(bSimulating && !bEditingConstraint);

        if (DetailsTarget != nullptr)
        {
            DetailsTable->DrawTree();
            if (const SPhysicsConstraintSetup* Constraint = GetSelectedConstraint())
            {
                DrawSoftLimitResponse(*Constraint);
            }
        }
        else
        {
            ImGui::TextDisabled("Select a bone with a body, or a constraint.");
        }

        ImGui::EndDisabled();
        ImGui::BeginDisabled(bSimulating);

        ImGui::Spacing();
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::SeparatorText("Physics Asset");
        ImGuiX::Font::PopFont();
        ImGui::Spacing();

        PropertyTable.DrawTree();

        ImGui::EndDisabled();
    }

    void FPhysicsAssetEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);

        if (ImGui::BeginMenu(LE_ICON_SKULL " Physics"))
        {
            if (bSimulating)
            {
                if (ImGui::MenuItem(LE_ICON_STOP " Stop Simulating", "Esc"))
                {
                    StopSimulation();
                }
            }
            else if (ImGui::MenuItem(LE_ICON_PLAY " Simulate"))
            {
                StartSimulation();
            }

            ImGui::Separator();

            bool bBodies = bDrawBodies;
            if (ImGui::MenuItem("Draw Bodies", nullptr, &bBodies))
            {
                bDrawBodies = bBodies;
            }

            bool bConstraints = bDrawConstraints;
            if (ImGui::MenuItem("Draw Constraints", nullptr, &bConstraints))
            {
                bDrawConstraints = bConstraints;
            }

            bool bBones = bDrawBoneLines;
            if (ImGui::MenuItem("Draw Bone Lines", nullptr, &bBones))
            {
                bDrawBoneLines = bBones;
            }

            ImGui::Separator();

            // Space cycles these in the viewport; scale is deliberately absent, the dots own size.
            if (ImGui::MenuItem("Move Body Frame", "Space", BodyGizmoOp == ImGuizmo::TRANSLATE))
            {
                BodyGizmoOp = ImGuizmo::TRANSLATE;
            }
            if (ImGui::MenuItem("Rotate Body Frame", "Space", BodyGizmoOp == ImGuizmo::ROTATE))
            {
                BodyGizmoOp = ImGuizmo::ROTATE;
            }

            ImGui::Separator();

            if (ImGui::MenuItem(LE_ICON_AUTO_FIX " Generate From Skeleton"))
            {
                GenerateFromSkeleton(true);
            }

            ImGui::EndMenu();
        }
    }

    void FPhysicsAssetEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Bodies",
            "Each body is a collision primitive bound to a bone. Right-click a bone in the tree to add or "
            "remove one. The selected body draws its frame axes so offsets can be dialed in.");
        DrawHelpTextRow("Generate",
            "Fits a capsule down every bone longer than the minimum length and joins each to its nearest "
            "ancestor body. Start here, then fix up by hand.");
        DrawHelpTextRow("Constraints",
            "A swing-twist joint limiting how far a child body rotates against its parent. The red cone is the "
            "elliptical swing limit and the red arc the twist limit; the amber cone and arc are where the soft "
            "limit starts pulling back. The selected constraint's dots drag those angles.");
        DrawHelpTextRow("Preview",
            "The mesh comes from the Skeleton's Preview Mesh and is drawn in bind pose. Bodies are authored "
            "against the bind pose, so what you see is what the ragdoll starts from.");
        DrawHelpTextRow("Viewport Editing",
            "Click a body in the viewport to select it. The orange dots resize it: one per box extent, or "
            "radius and half-height on a capsule. Drag a dot along its axis to set that dimension.");
        DrawHelpTextRow("Body Frame",
            "The gizmo on the selected body moves and rotates its frame relative to the bone, writing the "
            "Translation and Rotation offsets. Space swaps between move and rotate.");
        DrawHelpTextRow("Grabbing",
            "While simulating, Shift + left-drag pulls the body under the cursor with a spring. Fling it to "
            "check whether the joint limits hold up under real forces.");
    }

    void FPhysicsAssetEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftID = 0, CenterID = 0, RightID = 0, LeftBottomID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.28f, &RightID, &CenterID);
        ImGui::DockBuilderSplitNode(CenterID, ImGuiDir_Left, 0.26f, &LeftID, &CenterID);
        ImGui::DockBuilderSplitNode(LeftID, ImGuiDir_Down, 0.35f, &LeftBottomID, &LeftID);

        ImGui::DockBuilderDockWindow(*GetToolWindowName(ViewportWindowName), CenterID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(SkeletonTreeWindowName), LeftID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(ConstraintsWindowName), LeftBottomID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(PhysicsAssetDetailsWindowName), RightID);
    }
}
