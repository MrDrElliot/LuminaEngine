#include "PhysicsAssetEditorTool.h"

#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Core/Object/Package/Package.h"
#include "Physics/PhysicsScene.h"
#include "Physics/Ray/RayCast.h"
#include "World/Entity/Events/ImpulseEvent.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyTable.h"
#include "UI/Tools/Transactions/EditorTransaction.h"
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "imgui.h"

namespace Lumina
{
    static const char* SkeletonTreeWindowName   = "Skeleton Tree";
    static const char* ConstraintsWindowName    = "Constraints";
    static const char* DetailsWindowName        = "Details";

    static const FVector4 BodyColor             = FVector4(0.35f, 0.70f, 1.00f, 1.0f);
    static const FVector4 BodySelectedColor     = FVector4(1.00f, 0.75f, 0.15f, 1.0f);
    static const FVector4 ConstraintColor       = FVector4(0.40f, 1.00f, 0.55f, 1.0f);
    static const FVector4 ConstraintSelColor    = FVector4(1.00f, 0.45f, 0.25f, 1.0f);
    static const FVector4 BoneLineColor         = FVector4(0.35f, 0.35f, 0.40f, 1.0f);

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

    static void GatherChildOffsets(const FSkeletonResource& Resource, int32 BoneIndex, TVector<FVector3>& OutOffsets)
    {
        OutOffsets.clear();
        for (int32 ChildIndex : Resource.GetChildBones(BoneIndex))
        {
            OutOffsets.push_back(FVector3(Resource.GetBone(ChildIndex).LocalTransform[3]));
        }
    }

    // Sizes a body from where the bone's children sit, in bone space. False means the bone is too small
    // to be worth simulating and Body was left untouched.
    static bool ShapeBodyForBone(SPhysicsBodySetup& Body, const TVector<FVector3>& ChildOffsets, float MinBoneLength)
    {
        if (ChildOffsets.empty())
        {
            return false;
        }

        if (ChildOffsets.size() == 1)
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

        // Branching bones (pelvis, chest) get a sphere at the joint. Averaging the child OFFSETS cancels
        // out here -- a pelvis's two thighs point opposite ways -- which left the bone with no body, and a
        // bodiless bone is pinned to its bind-pose local while everything below it falls away.
        float MeanDistance = 0.0f;
        for (const FVector3& Offset : ChildOffsets)
        {
            MeanDistance += Math::Length(Offset);
        }
        MeanDistance /= (float)ChildOffsets.size();

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

    // Parameter along the infinite line (Anchor + t*Axis) closest to the ray. False when the two are
    // near-parallel, where t runs away to nothing useful.
    static bool ClosestParamOnAxisToRay(const FVector3& Anchor, const FVector3& Axis,
                                        const FVector3& RayOrigin, const FVector3& RayDirection, float& OutT)
    {
        const FVector3 ToAnchor = Anchor - RayOrigin;
        const float AxisDotRay = Math::Dot(Axis, RayDirection);
        const float Denominator = 1.0f - AxisDotRay * AxisDotRay;
        if (Math::Abs(Denominator) < 1e-5f)
        {
            return false;
        }

        OutT = (AxisDotRay * Math::Dot(RayDirection, ToAnchor) - Math::Dot(Axis, ToAnchor)) / Denominator;
        return true;
    }

    static bool RayHitsSphere(const FVector3& Origin, const FVector3& Direction, const FVector3& Center, float Radius, float& OutT)
    {
        const FVector3 ToOrigin = Origin - Center;
        const float B = Math::Dot(ToOrigin, Direction);
        const float C = Math::Dot(ToOrigin, ToOrigin) - Radius * Radius;

        if (C > 0.0f && B > 0.0f)
        {
            return false;
        }

        const float Discriminant = B * B - C;
        if (Discriminant < 0.0f)
        {
            return false;
        }

        OutT = Math::Max(0.0f, -B - Math::Sqrt(Discriminant));
        return true;
    }

    // Hit when the ray passes within Radius of the capsule's core segment; OutT is the ray parameter at
    // closest approach, which is close enough for ordering overlapping bodies under the cursor.
    static bool RayHitsCapsule(const FVector3& Origin, const FVector3& Direction,
                               const FVector3& SegmentStart, const FVector3& SegmentEnd, float Radius, float& OutT)
    {
        const FVector3 Segment = SegmentEnd - SegmentStart;
        const float SegmentLengthSq = Math::LengthSquared(Segment);
        if (SegmentLengthSq < 1e-8f)
        {
            return RayHitsSphere(Origin, Direction, SegmentStart, Radius, OutT);
        }

        const FVector3 ToStart = Origin - SegmentStart;
        const float DirDotSeg = Math::Dot(Direction, Segment);
        const float DirDotToStart = Math::Dot(Direction, ToStart);
        const float SegDotToStart = Math::Dot(Segment, ToStart);

        float SegmentParam = 0.0f;
        const float Denominator = SegmentLengthSq - DirDotSeg * DirDotSeg;
        if (Math::Abs(Denominator) > 1e-6f)
        {
            SegmentParam = Math::Clamp((SegDotToStart - DirDotSeg * DirDotToStart) / Denominator, 0.0f, 1.0f);
        }

        // Ray parameter for the closest point on the (clamped) segment, then re-clamp behind the eye.
        float RayParam = DirDotSeg * SegmentParam - DirDotToStart;
        if (RayParam < 0.0f)
        {
            RayParam = 0.0f;
            SegmentParam = Math::Clamp(SegDotToStart / SegmentLengthSq, 0.0f, 1.0f);
        }

        const FVector3 PointOnRay = Origin + Direction * RayParam;
        const FVector3 PointOnSegment = SegmentStart + Segment * SegmentParam;
        if (Math::LengthSquared(PointOnRay - PointOnSegment) > Radius * Radius)
        {
            return false;
        }

        OutT = RayParam;
        return true;
    }

    static bool RayHitsBox(const FVector3& Origin, const FVector3& Direction, const FMatrix4& BodyMatrix,
                           const FVector3& HalfExtent, float& OutT)
    {
        const FMatrix4 InverseBody = Math::Inverse(BodyMatrix);
        const FVector3 LocalOrigin = FVector3(InverseBody * FVector4(Origin, 1.0f));
        const FVector3 LocalDirection = FVector3(InverseBody * FVector4(Direction, 0.0f));

        const float OriginAxis[3]    = { LocalOrigin.x, LocalOrigin.y, LocalOrigin.z };
        const float DirectionAxis[3] = { LocalDirection.x, LocalDirection.y, LocalDirection.z };
        const float ExtentAxis[3]    = { HalfExtent.x, HalfExtent.y, HalfExtent.z };

        float TMin = 0.0f;
        float TMax = 1e30f;

        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            if (Math::Abs(DirectionAxis[Axis]) < 1e-6f)
            {
                if (OriginAxis[Axis] < -ExtentAxis[Axis] || OriginAxis[Axis] > ExtentAxis[Axis])
                {
                    return false;
                }
                continue;
            }

            float TNear = (-ExtentAxis[Axis] - OriginAxis[Axis]) / DirectionAxis[Axis];
            float TFar  = ( ExtentAxis[Axis] - OriginAxis[Axis]) / DirectionAxis[Axis];
            if (TNear > TFar)
            {
                eastl::swap(TNear, TFar);
            }

            TMin = Math::Max(TMin, TNear);
            TMax = Math::Min(TMax, TFar);
            if (TMin > TMax)
            {
                return false;
            }
        }

        // Back to a world distance so overlapping bodies of different shapes sort against each other.
        const FVector3 WorldHit = FVector3(BodyMatrix * FVector4(LocalOrigin + LocalDirection * TMin, 1.0f));
        OutT = Math::Length(WorldHit - Origin);
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
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
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
        });

        SetupTreeContext();

        CreateToolWindow(SkeletonTreeWindowName, [this](bool) { DrawSkeletonTreeWindow(); });
        CreateToolWindow(ConstraintsWindowName, [this](bool) { DrawConstraintsWindow(); });
        CreateToolWindow(DetailsWindowName, [this](bool) { DrawDetailsWindow(); });

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
                const char* Icon = (BodyIndex != INDEX_NONE) ? ShapeIcon(PhysicsAsset->Bodies[BodyIndex].Shape) : LE_ICON_BONE;

                // The glyph goes in the label as well as IconText: the widget draws IconText OVER the row
                // instead of reserving space ahead of it, so without the prefix it lands on the name.
                const FString Label = FString(Icon) + "  " + BoneName.c_str();

                FTreeNodeID Node = Tree.CreateNode(ParentNode, FStringView(Label.c_str()));
                Tree.EmplaceUserData<int32>(Node, BoneIndex);
                Tree.Get<FTreeNodeState>(Node).bExpanded = true;

                {
                    FTreeNodeDisplay& Display = Tree.Get<FTreeNodeDisplay>(Node);
                    Display.IconText = Icon;
                    if (BodyIndex != INDEX_NONE)
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
            return BoneIndex >= 0 && BoneIndex < (int32)BodySubtreeMask.size() && BodySubtreeMask[BoneIndex] != 0;
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

            // Selecting a bone selects its body when it has one; otherwise the details panel goes
            // empty rather than keeping a stale body on screen.
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

            // Restructuring the body list under a live ragdoll leaves the simulated bodies describing a
            // setup that no longer exists.
            ImGui::BeginDisabled(bSimulating);

            if (BodyIndex == INDEX_NONE)
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

        ClearSelection();
        SelectedBone = NAME_None;
        RebuildBodySubtreeMask();
        BoneListView.MarkTreeDirty();

        if (!World.IsValid())
        {
            return;
        }

        if (MeshEntity != entt::null)
        {
            World->DestroyEntity(MeshEntity);
            MeshEntity = entt::null;
        }

        CSkeleton* Skeleton = GetSkeleton();
        if (Skeleton == nullptr || !Skeleton->PreviewMesh.IsValid())
        {
            return;
        }

        MeshEntity = World->ConstructEntity("PreviewMesh");
        SSkeletalMeshComponent& MeshComponent = World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity);
        MeshComponent.SetSkeletalMesh(Skeleton->PreviewMesh);
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

    void FPhysicsAssetEditorTool::OnPostUndoRedo()
    {
        FAssetEditorTool::OnPostUndoRedo();

        // A restore rewrites Bodies and Constraints wholesale, so the details table's pointer into an
        // element and the tree's body icons both describe data that no longer exists. Undoing an added
        // body can also leave the selection index past the end.
        DetailsTarget = nullptr;
        DetailsType = nullptr;

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        if (SelectedBodyIndex >= (int32)PhysicsAsset->Bodies.size()
            || SelectedConstraintIndex >= (int32)PhysicsAsset->Constraints.size())
        {
            ClearSelection();
        }

        SyncDetailsTable();
        RebuildBodySubtreeMask();
        BoneListView.MarkTreeDirty();
    }

    void FPhysicsAssetEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        if (!World.IsValid())
        {
            return;
        }

        // Skeleton is an editable property, so it can be swapped from the details panel at any time.
        if (CachedSkeleton.Get() != GetSkeleton())
        {
            RefreshForSkeletonChange();
        }

        SyncDetailsTable();

        if (World->GetRenderer() != nullptr)
        {
            World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;
        }

        // The authoring overlays are drawn from the bind pose, which the live ragdoll has left behind, so
        // they are replaced by a live one rather than shown alongside it.
        if (bSimulating)
        {
            DrawSimulationOverlay();
            return;
        }

        RebuildBoneWorldTransforms();

        if (BoneWorldTransforms.empty())
        {
            return;
        }

        if (bDrawBoneLines)
        {
            FSkeletonResource* Resource = GetSkeletonResource();
            for (int32 i = 0; i < Resource->GetNumBones(); ++i)
            {
                const int32 ParentIndex = Resource->GetBone(i).ParentIndex;
                if (ParentIndex != INDEX_NONE)
                {
                    World->DrawLine(FVector3(BoneWorldTransforms[ParentIndex][3]), FVector3(BoneWorldTransforms[i][3]), BoneLineColor, 1.0f, false);
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
        BoneWorldTransforms.clear();

        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr || Resource->GetNumBones() == 0)
        {
            return;
        }

        FMatrix4 EntityMatrix = FMatrix4(1.0f);
        if (MeshEntity != entt::null)
        {
            EntityMatrix = World->GetComponent<STransformComponent>(MeshEntity).GetWorldMatrix();
        }

        BoneWorldTransforms.resize(Resource->GetNumBones());
        for (int32 i = 0; i < Resource->GetNumBones(); ++i)
        {
            const FSkeletonResource::FBoneInfo& Bone = Resource->GetBone(i);
            if (Bone.ParentIndex == INDEX_NONE)
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
        BodySubtreeMask.clear();

        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr)
        {
            return;
        }

        BodySubtreeMask.resize(Resource->GetNumBones(), 0);

        for (const SPhysicsBodySetup& Body : GetAsset<CPhysicsAsset>()->Bodies)
        {
            int32 Index = Resource->FindBoneIndex(Body.BoneName);
            while (Index >= 0 && Index < (int32)BodySubtreeMask.size())
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
        if (BoneIndex == INDEX_NONE || BoneIndex >= (int32)BoneWorldTransforms.size())
        {
            return FMatrix4(1.0f);
        }

        const FMatrix4 Offset = Math::Translate(FMatrix4(1.0f), Body.TranslationOffset) * Math::ToMatrix4(FQuat(Math::Radians(Body.RotationOffset)));
        return BoneWorldTransforms[BoneIndex] * Offset;
    }

    void FPhysicsAssetEditorTool::DrawBodies()
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        for (int32 i = 0; i < (int32)PhysicsAsset->Bodies.size(); ++i)
        {
            const SPhysicsBodySetup& Body = PhysicsAsset->Bodies[i];
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
                    World->DrawCapsule(Center - Axis * Body.HalfHeight, Center + Axis * Body.HalfHeight, Body.Radius, Color, 16, Thickness, false);
                }
                break;

            case ERagdollBodyShape::Sphere:
                World->DrawSphere(Center, Body.Radius, Color, 16, Thickness, false);
                break;

            case ERagdollBodyShape::Box:
                World->DrawBox(Center, Body.HalfExtent, Math::ToQuat(BodyMatrix), Color, Thickness, false);
                break;
            }

            // Body frame axes only on the selection: authoring RotationOffset is impossible without
            // seeing which way the frame points, and drawing them for every body is unreadable.
            if (bSelected)
            {
                constexpr float AxisLength = 0.12f;
                World->DrawLine(Center, Center + Math::Normalize(FVector3(BodyMatrix[0])) * AxisLength, FVector4(1.0f, 0.2f, 0.2f, 1.0f), 2.5f, false);
                World->DrawLine(Center, Center + Math::Normalize(FVector3(BodyMatrix[1])) * AxisLength, FVector4(0.2f, 1.0f, 0.2f, 1.0f), 2.5f, false);
                World->DrawLine(Center, Center + Math::Normalize(FVector3(BodyMatrix[2])) * AxisLength, FVector4(0.2f, 0.4f, 1.0f, 1.0f), 2.5f, false);
            }
        }
    }

    void FPhysicsAssetEditorTool::DrawConstraints()
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr)
        {
            return;
        }

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        for (int32 i = 0; i < (int32)PhysicsAsset->Constraints.size(); ++i)
        {
            const SPhysicsConstraintSetup& Constraint = PhysicsAsset->Constraints[i];

            const int32 ChildIndex = Resource->FindBoneIndex(Constraint.ChildBone);
            if (ChildIndex == INDEX_NONE || ChildIndex >= (int32)BoneWorldTransforms.size())
            {
                continue;
            }

            const bool bSelected = (SelectionMode == EPhysicsAssetSelection::Constraint && i == SelectedConstraintIndex);
            const FVector4 Color = bSelected ? ConstraintSelColor : ConstraintColor;
            const float Thickness = bSelected ? 3.0f : 1.5f;

            const FMatrix4 ChildMatrix = BoneWorldTransforms[ChildIndex];
            const FVector3 Pivot = FVector3(ChildMatrix[3]);

            const int32 ParentIndex = Resource->FindBoneIndex(Constraint.ParentBone);
            if (ParentIndex != INDEX_NONE && ParentIndex < (int32)BoneWorldTransforms.size())
            {
                World->DrawLine(Pivot, FVector3(BoneWorldTransforms[ParentIndex][3]), Color, Thickness, false);
            }

            const FVector3 TwistAxis = Math::Normalize(FVector3(ChildMatrix[1]));
            const float ConeLength = bSelected ? 0.18f : 0.12f;
            World->DrawCone(Pivot, TwistAxis, Math::Radians(Constraint.Swing1LimitDegrees), ConeLength, Color, 16, 4, Thickness, false);
        }
    }

    bool FPhysicsAssetEditorTool::BuildViewportRay(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize,
                                                   const ImVec2& ScreenPos, FVector3& OutOrigin, FVector3& OutDirection)
    {
        SCameraComponent* Camera = World->GetActiveCamera();
        if (Camera == nullptr)
        {
            return false;
        }

        const float LocalX = ScreenPos.x - ViewportOrigin.x;
        const float LocalY = ScreenPos.y - ViewportOrigin.y;
        if (LocalX < 0.0f || LocalY < 0.0f || LocalX >= ViewportSize.x || LocalY >= ViewportSize.y)
        {
            return false;
        }

        // The camera projection bakes Vulkan's +Y-down NDC; flip it back before unprojecting.
        FMatrix4 Projection = Camera->GetProjectionMatrix();
        Projection[1][1] *= -1.0f;
        const FMatrix4 InverseViewProjection = Math::Inverse(Projection * Camera->GetViewMatrix());

        const float NdcX = (LocalX / ViewportSize.x) * 2.0f - 1.0f;
        const float NdcY = 1.0f - (LocalY / ViewportSize.y) * 2.0f;

        const FVector4 FarPoint = InverseViewProjection * FVector4(NdcX, NdcY, 1.0f, 1.0f);
        if (Math::Abs(FarPoint.w) < 1e-6f)
        {
            return false;
        }

        OutOrigin = Camera->GetPosition();
        OutDirection = Math::Normalize(FVector3(FarPoint) / FarPoint.w - OutOrigin);
        return true;
    }

    bool FPhysicsAssetEditorTool::ProjectToScreen(const FMatrix4& ViewProj, const ImVec2& ViewportOrigin,
                                                  const ImVec2& ViewportSize, const FVector3& WorldPosition, ImVec2& OutScreen)
    {
        const FVector4 Clip = ViewProj * FVector4(WorldPosition, 1.0f);
        if (Clip.w <= 1e-6f)
        {
            return false;
        }

        const FVector3 Ndc = FVector3(Clip) / Clip.w;
        OutScreen = ImVec2(ViewportOrigin.x + (Ndc.x * 0.5f + 0.5f) * ViewportSize.x,
                           ViewportOrigin.y + (0.5f - Ndc.y * 0.5f) * ViewportSize.y);
        return true;
    }

    int32 FPhysicsAssetEditorTool::PickBody(const FVector3& RayOrigin, const FVector3& RayDirection)
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        int32 BestBody = INDEX_NONE;
        float BestDistance = 1e30f;

        for (int32 i = 0; i < (int32)PhysicsAsset->Bodies.size(); ++i)
        {
            const SPhysicsBodySetup& Body = PhysicsAsset->Bodies[i];
            const FMatrix4 BodyMatrix = GetBodyWorldMatrix(Body);
            const FVector3 Center = FVector3(BodyMatrix[3]);

            float Distance = 0.0f;
            bool bHit = false;

            switch (Body.Shape)
            {
            case ERagdollBodyShape::Capsule:
                {
                    const FVector3 Axis = Math::Normalize(FVector3(BodyMatrix[1]));
                    bHit = RayHitsCapsule(RayOrigin, RayDirection, Center - Axis * Body.HalfHeight,
                                          Center + Axis * Body.HalfHeight, Body.Radius, Distance);
                }
                break;

            case ERagdollBodyShape::Sphere:
                bHit = RayHitsSphere(RayOrigin, RayDirection, Center, Body.Radius, Distance);
                break;

            case ERagdollBodyShape::Box:
                bHit = RayHitsBox(RayOrigin, RayDirection, BodyMatrix, Body.HalfExtent, Distance);
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

    void FPhysicsAssetEditorTool::GatherBodyHandles(TVector<FPhysicsHandle>& OutHandles)
    {
        OutHandles.clear();

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        if (SelectionMode != EPhysicsAssetSelection::Body
            || SelectedBodyIndex < 0 || SelectedBodyIndex >= (int32)PhysicsAsset->Bodies.size())
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
            OutHandles.push_back({ EPhysicsBodyHandle::Radius,     Center + AxisX * Body.Radius,     AxisX, Center });
            OutHandles.push_back({ EPhysicsBodyHandle::HalfHeight, Center + AxisY * Body.HalfHeight, AxisY, Center });
            break;

        case ERagdollBodyShape::Sphere:
            OutHandles.push_back({ EPhysicsBodyHandle::Radius, Center + AxisX * Body.Radius, AxisX, Center });
            break;

        case ERagdollBodyShape::Box:
            OutHandles.push_back({ EPhysicsBodyHandle::ExtentX, Center + AxisX * Body.HalfExtent.x, AxisX, Center });
            OutHandles.push_back({ EPhysicsBodyHandle::ExtentY, Center + AxisY * Body.HalfExtent.y, AxisY, Center });
            OutHandles.push_back({ EPhysicsBodyHandle::ExtentZ, Center + AxisZ * Body.HalfExtent.z, AxisZ, Center });
            break;
        }
    }

    void FPhysicsAssetEditorTool::ApplyHandleDrag(const FPhysicsHandle& Handle, const FVector3& RayOrigin, const FVector3& RayDirection)
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        if (SelectedBodyIndex < 0 || SelectedBodyIndex >= (int32)PhysicsAsset->Bodies.size())
        {
            return;
        }

        float AxisDistance = 0.0f;
        if (!ClosestParamOnAxisToRay(Handle.Anchor, Handle.Axis, RayOrigin, RayDirection, AxisDistance))
        {
            return;
        }

        SPhysicsBodySetup& Body = PhysicsAsset->Bodies[SelectedBodyIndex];

        // Radii and extents are clamped away from zero; a half-height of zero is a legitimate sphere-capsule.
        const float Positive = Math::Max(AxisDistance, 0.001f);

        switch (Handle.Type)
        {
        case EPhysicsBodyHandle::Radius:     Body.Radius = Positive; break;
        case EPhysicsBodyHandle::HalfHeight: Body.HalfHeight = Math::Max(AxisDistance, 0.0f); break;
        case EPhysicsBodyHandle::ExtentX:    Body.HalfExtent.x = Positive; break;
        case EPhysicsBodyHandle::ExtentY:    Body.HalfExtent.y = Positive; break;
        case EPhysicsBodyHandle::ExtentZ:    Body.HalfExtent.z = Positive; break;
        default: return;
        }

        // No MarkDirty on the details table: its rows read through to the struct, so the numbers follow the
        // drag on their own and rebuilding the tree every frame of a drag is pure cost.
        NotifyAssetDataChanged();
    }

    void FPhysicsAssetEditorTool::BeginAssetTransaction(FName Name)
    {
        FTransactionManager& Manager = GetTransactionManager();
        Manager.BeginTransaction(Name);
        Manager.Record(MakeUnique<FObjectSnapshotCommand>(Asset.Get(), Name));
    }

    void FPhysicsAssetEditorTool::EndAssetTransaction()
    {
        // The command captures its after-image here and drops itself if the drag changed nothing.
        GetTransactionManager().CommitTransaction();
    }

    void FPhysicsAssetEditorTool::CloseOpenDragTransaction()
    {
        if (bHandleTransactionOpen || bGizmoTransactionOpen)
        {
            bHandleTransactionOpen = false;
            bGizmoTransactionOpen = false;
            EndAssetTransaction();
        }
    }

    void FPhysicsAssetEditorTool::ApplyBodyGizmo(const FMatrix4& NewBodyMatrix)
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        if (Resource == nullptr || SelectedBodyIndex < 0 || SelectedBodyIndex >= (int32)PhysicsAsset->Bodies.size())
        {
            return;
        }

        SPhysicsBodySetup& Body = PhysicsAsset->Bodies[SelectedBodyIndex];

        const int32 BoneIndex = Resource->FindBoneIndex(Body.BoneName);
        if (BoneIndex == INDEX_NONE || BoneIndex >= (int32)BoneWorldTransforms.size())
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

        if (Resource == nullptr || BodyIndex < 0 || BodyIndex >= (int32)PhysicsAsset->Bodies.size())
        {
            return;
        }

        SelectedBone = PhysicsAsset->Bodies[BodyIndex].BoneName;

        const int32 BoneIndex = Resource->FindBoneIndex(SelectedBone);
        if (BoneIndex == INDEX_NONE || BoneListView.IsDirty())
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
        constexpr uint32 InvalidBody = 0xFFFFFFFF;
        constexpr float GrabReach = 100.0f;
        constexpr float GrabStiffness = 400.0f;
        constexpr float GrabDamping = 30.0f;

        Physics::IPhysicsScene* Scene = World->GetPhysicsScene();
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

        const ImVec2 MousePos = ImGui::GetMousePos();
        FVector3 RayOrigin, RayDirection;
        if (!BuildViewportRay(ViewportOrigin, ViewportSize, MousePos, RayOrigin, RayDirection))
        {
            return;
        }

        if (GrabbedBodyID == InvalidBody)
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
            if (!Hit.has_value() || (entt::entity)Hit->Entity == FloorBodyEntity)
            {
                return;
            }

            GrabbedBodyID = (uint32)Hit->BodyID;
            GrabDistance = Math::Length(Hit->Location - RayOrigin);
            GrabLocalOffset = Math::Inverse(Scene->GetBodyRotation(GrabbedBodyID)) * (Hit->Location - Scene->GetBodyPosition(GrabbedBodyID));
            return;
        }

        const FVector3 Target = RayOrigin + RayDirection * GrabDistance;
        const FVector3 AttachPoint = Scene->GetBodyPosition(GrabbedBodyID) + Scene->GetBodyRotation(GrabbedBodyID) * GrabLocalOffset;

        // Critically-damped-ish spring: the damping term is what stops the body oscillating wildly once the
        // cursor stops, which a pure position spring does badly at these stiffnesses.
        const FVector3 Force = (Target - AttachPoint) * GrabStiffness
                             - Scene->GetVelocityAtPoint(GrabbedBodyID, AttachPoint) * GrabDamping;

        SAddForceAtPositionEvent ForceEvent;
        ForceEvent.BodyID = GrabbedBodyID;
        ForceEvent.Force = Force;
        ForceEvent.Position = AttachPoint;

        Scene->ActivateBody(GrabbedBodyID);
        Scene->OnAddForceAtPositionEvent(ForceEvent);

        World->DrawLine(AttachPoint, Target, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 2.5f, false);
        World->DrawSphere(Target, 0.035f, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 10, 2.0f, false);
    }

    void FPhysicsAssetEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        // Read BEFORE the base call. DrawViewport parks the cursor on the overlay origin, and every other
        // tool takes it straight from there -- but the base override submits a Dummy, which advances the
        // cursor to the next line. Reading after it displaced the origin, which pushed every mouse
        // position off and made anything clicked near the top of the viewport fail the bounds test outright.
        const ImVec2 ViewportOrigin = ImGui::GetCursorScreenPos();

        FAssetEditorTool::DrawViewportOverlayElements(UpdateContext, ViewportTexture, ViewportSize);

        SCameraComponent* Camera = World.IsValid() ? World->GetActiveCamera() : nullptr;
        if (Camera == nullptr)
        {
            ActiveHandle = EPhysicsBodyHandle::None;
            return;
        }

        if (bSimulating)
        {
            // Gated on viewport focus/hover like the base's F11, so Esc typed into another panel does not
            // kill the run out from under you.
            if ((bViewportFocused || bViewportHovered) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                StopSimulation();
                return;
            }

            ActiveHandle = EPhysicsBodyHandle::None;
            CloseOpenDragTransaction();
            UpdateSimulationGrab(ViewportOrigin, ViewportSize);
            return;
        }

        if (BoneWorldTransforms.empty())
        {
            return;
        }

        if (bViewportHovered && ImGui::IsKeyPressed(ImGuiKey_Space))
        {
            BodyGizmoOp = (BodyGizmoOp == ImGuizmo::TRANSLATE) ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE;
        }

        FMatrix4 ViewMatrix = Camera->GetViewMatrix();
        FMatrix4 Projection = Camera->GetProjectionMatrix();
        Projection[1][1] *= -1.0f;
        const FMatrix4 ViewProj = Projection * ViewMatrix;

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        const bool bBodySelected = SelectionMode == EPhysicsAssetSelection::Body
                                && SelectedBodyIndex >= 0 && SelectedBodyIndex < (int32)PhysicsAsset->Bodies.size();

        // Frame gizmo first, so it owns the cursor before the resize dots or picking see it.
        bool bGizmoOwnsInput = false;
        if (bBodySelected)
        {
            ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
            ImGuizmo::SetRect(ViewportOrigin.x, ViewportOrigin.y, ViewportSize.x, ViewportSize.y);

            FMatrix4 BodyMatrix = GetBodyWorldMatrix(PhysicsAsset->Bodies[SelectedBodyIndex]);

            const bool bGizmoInert = ShouldSuppressViewportClickInput() && !ImGuizmo::IsUsing();
            if (bGizmoInert)
            {
                ImGuizmo::Enable(false);
            }

            ImGuizmo::Manipulate(Math::ValuePtr(ViewMatrix), Math::ValuePtr(Projection),
                BodyGizmoOp, ImGuizmo::LOCAL, Math::ValuePtr(BodyMatrix));

            if (bGizmoInert)
            {
                ImGuizmo::Enable(true);
            }

            if (ImGuizmo::IsUsing())
            {
                if (!bGizmoTransactionOpen)
                {
                    BeginAssetTransaction(BodyGizmoOp == ImGuizmo::ROTATE ? "Rotate Body Frame" : "Move Body Frame");
                    bGizmoTransactionOpen = true;
                }

                ApplyBodyGizmo(BodyMatrix);
            }
            else if (bGizmoTransactionOpen)
            {
                bGizmoTransactionOpen = false;
                EndAssetTransaction();
            }

            bGizmoOwnsInput = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
        }

        if (bGizmoOwnsInput && ActiveHandle == EPhysicsBodyHandle::None)
        {
            return;
        }

        TVector<FPhysicsHandle> Handles;
        GatherBodyHandles(Handles);

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const ImVec2 MousePos = ImGui::GetMousePos();

        constexpr float HandleRadius = 6.0f;
        constexpr float GrabRadius = 10.0f;

        int32 HoveredHandle = INDEX_NONE;

        for (int32 i = 0; i < (int32)Handles.size(); ++i)
        {
            ImVec2 Screen;
            if (!ProjectToScreen(ViewProj, ViewportOrigin, ViewportSize, Handles[i].Position, Screen))
            {
                continue;
            }

            const float DX = MousePos.x - Screen.x;
            const float DY = MousePos.y - Screen.y;
            const bool bHot = (ActiveHandle == Handles[i].Type)
                           || (ActiveHandle == EPhysicsBodyHandle::None && (DX * DX + DY * DY) <= GrabRadius * GrabRadius);

            if (bHot && ActiveHandle == EPhysicsBodyHandle::None)
            {
                HoveredHandle = i;
            }

            DrawList->AddCircleFilled(Screen, bHot ? HandleRadius + 1.5f : HandleRadius,
                bHot ? IM_COL32(255, 200, 60, 255) : IM_COL32(90, 180, 255, 235));
            DrawList->AddCircle(Screen, bHot ? HandleRadius + 1.5f : HandleRadius, IM_COL32(15, 15, 20, 220), 0, 1.5f);
        }

        const bool bCanInteract = bViewportHovered && !ShouldSuppressViewportClickInput();

        if (ActiveHandle != EPhysicsBodyHandle::None)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                ActiveHandle = EPhysicsBodyHandle::None;
                if (bHandleTransactionOpen)
                {
                    bHandleTransactionOpen = false;
                    EndAssetTransaction();
                }
            }
            else
            {
                for (const FPhysicsHandle& Handle : Handles)
                {
                    if (Handle.Type != ActiveHandle)
                    {
                        continue;
                    }

                    FVector3 RayOrigin, RayDirection;
                    if (BuildViewportRay(ViewportOrigin, ViewportSize, MousePos, RayOrigin, RayDirection))
                    {
                        ApplyHandleDrag(Handle, RayOrigin, RayDirection);
                    }
                    break;
                }
            }
            return;
        }

        if (!bCanInteract)
        {
            return;
        }

        if (HoveredHandle != INDEX_NONE && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ActiveHandle = Handles[HoveredHandle].Type;
            BeginAssetTransaction("Resize Body");
            bHandleTransactionOpen = true;
            return;
        }

        // Pick on release rather than press, and only when the mouse barely moved, so a click that turned
        // into a camera drag does not also reselect.
        if (HoveredHandle == INDEX_NONE && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            const ImVec2 Drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (Drag.x * Drag.x + Drag.y * Drag.y <= 16.0f)
            {
                FVector3 RayOrigin, RayDirection;
                if (BuildViewportRay(ViewportOrigin, ViewportSize, MousePos, RayOrigin, RayDirection))
                {
                    const int32 Picked = PickBody(RayOrigin, RayDirection);
                    SelectBody(Picked);
                    if (Picked != INDEX_NONE)
                    {
                        SyncTreeSelectionToBody(Picked);
                    }
                }
            }
        }
    }

    int32 FPhysicsAssetEditorTool::AddBodyForBone(const FName& BoneName)
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        const int32 Existing = PhysicsAsset->FindBodyIndex(BoneName);
        if (Existing != INDEX_NONE)
        {
            return Existing;
        }

        BeginAssetTransaction("Add Body");

        SPhysicsBodySetup& Body = PhysicsAsset->Bodies.emplace_back();
        Body.BoneName = BoneName;

        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource != nullptr)
        {
            const int32 BoneIndex = Resource->FindBoneIndex(BoneName);
            if (BoneIndex != INDEX_NONE)
            {
                // No minimum here: an explicit add always produces a body, even on a stub bone.
                TVector<FVector3> ChildOffsets;
                GatherChildOffsets(*Resource, BoneIndex, ChildOffsets);
                ShapeBodyForBone(Body, ChildOffsets, 0.0f);
            }
        }

        NotifyAssetDataChanged();
        RebuildBodySubtreeMask();
        BoneListView.MarkTreeDirty();
        EndAssetTransaction();

        return (int32)PhysicsAsset->Bodies.size() - 1;
    }

    void FPhysicsAssetEditorTool::RemoveBodyAt(int32 BodyIndex)
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        if (BodyIndex < 0 || BodyIndex >= (int32)PhysicsAsset->Bodies.size())
        {
            return;
        }

        BeginAssetTransaction("Remove Body");

        const FName BoneName = PhysicsAsset->Bodies[BodyIndex].BoneName;
        PhysicsAsset->Bodies.erase(PhysicsAsset->Bodies.begin() + BodyIndex);

        // A joint referencing a body that no longer exists cannot be built at runtime.
        for (int32 i = (int32)PhysicsAsset->Constraints.size() - 1; i >= 0; --i)
        {
            if (PhysicsAsset->Constraints[i].ChildBone == BoneName || PhysicsAsset->Constraints[i].ParentBone == BoneName)
            {
                PhysicsAsset->Constraints.erase(PhysicsAsset->Constraints.begin() + i);
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
        if (PhysicsAsset->Bodies.empty() && PhysicsAsset->Constraints.empty())
        {
            return;
        }

        BeginAssetTransaction("Clear Bodies");

        PhysicsAsset->Bodies.clear();
        PhysicsAsset->Constraints.clear();

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
            return INDEX_NONE;
        }

        const int32 BoneIndex = Resource->FindBoneIndex(ChildBone);
        if (BoneIndex == INDEX_NONE)
        {
            return INDEX_NONE;
        }

        const FName ParentBone = FindAncestorBodyBone(Resource->GetBone(BoneIndex).ParentIndex);
        if (ParentBone == NAME_None)
        {
            return INDEX_NONE;
        }

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        for (int32 i = 0; i < (int32)PhysicsAsset->Constraints.size(); ++i)
        {
            if (PhysicsAsset->Constraints[i].ChildBone == ChildBone)
            {
                return i;
            }
        }

        BeginAssetTransaction("Add Constraint");

        SPhysicsConstraintSetup& Constraint = PhysicsAsset->Constraints.emplace_back();
        Constraint.ChildBone = ChildBone;
        Constraint.ParentBone = ParentBone;

        NotifyAssetDataChanged();
        EndAssetTransaction();

        return (int32)PhysicsAsset->Constraints.size() - 1;
    }

    void FPhysicsAssetEditorTool::RemoveConstraintAt(int32 ConstraintIndex)
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        if (ConstraintIndex < 0 || ConstraintIndex >= (int32)PhysicsAsset->Constraints.size())
        {
            return;
        }

        BeginAssetTransaction("Remove Constraint");

        PhysicsAsset->Constraints.erase(PhysicsAsset->Constraints.begin() + ConstraintIndex);

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
            if (PhysicsAsset->FindBodyIndex(BoneName) != INDEX_NONE)
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
            PhysicsAsset->Bodies.clear();
            PhysicsAsset->Constraints.clear();
            ClearSelection();
        }

        for (int32 i = 0; i < Resource->GetNumBones(); ++i)
        {
            const FName BoneName = Resource->GetBone(i).Name;
            if (PhysicsAsset->FindBodyIndex(BoneName) != INDEX_NONE)
            {
                continue;
            }

            // Leaf bones (fingertips, twist ends) have no length to fit a shape to; a body per one of
            // those buries the ragdoll in tiny shapes nobody wants to simulate.
            TVector<FVector3> ChildOffsets;
            GatherChildOffsets(*Resource, i, ChildOffsets);

            SPhysicsBodySetup Candidate;
            Candidate.BoneName = BoneName;
            if (!ShapeBodyForBone(Candidate, ChildOffsets, GenerateMinBoneLength))
            {
                continue;
            }

            PhysicsAsset->Bodies.push_back(Candidate);
        }

        // Second pass: every body exists now, so the ancestor walk sees the whole set.
        for (int32 i = 0; i < (int32)PhysicsAsset->Bodies.size(); ++i)
        {
            const FName ChildBone = PhysicsAsset->Bodies[i].BoneName;

            const int32 BoneIndex = Resource->FindBoneIndex(ChildBone);
            if (BoneIndex == INDEX_NONE)
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

            SPhysicsConstraintSetup& Constraint = PhysicsAsset->Constraints.emplace_back();
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
        SelectedConstraintIndex = INDEX_NONE;
        SelectedBodyIndex = BodyIndex;
        SelectionMode = (BodyIndex != INDEX_NONE) ? EPhysicsAssetSelection::Body : EPhysicsAssetSelection::None;
    }

    void FPhysicsAssetEditorTool::SelectConstraint(int32 ConstraintIndex)
    {
        SelectedBodyIndex = INDEX_NONE;
        SelectedConstraintIndex = ConstraintIndex;
        SelectionMode = (ConstraintIndex != INDEX_NONE) ? EPhysicsAssetSelection::Constraint : EPhysicsAssetSelection::None;
    }

    void FPhysicsAssetEditorTool::ClearSelection()
    {
        SelectionMode = EPhysicsAssetSelection::None;
        SelectedBodyIndex = INDEX_NONE;
        SelectedConstraintIndex = INDEX_NONE;
    }

    void FPhysicsAssetEditorTool::DrawSimulationOverlay()
    {
        ++SimulationFrames;

        if (FloorBodyEntity != entt::null)
        {
            const STransformComponent& FloorTransform = World->GetComponent<STransformComponent>(FloorBodyEntity);
            if (const SBoxColliderComponent* FloorBox = World->TryGetComponent<SBoxColliderComponent>(FloorBodyEntity))
            {
                World->DrawBox(FloorTransform.GetLocation(), FloorBox->HalfExtent, FloorTransform.GetRotation(),
                    FVector4(0.25f, 1.0f, 0.45f, 1.0f), 2.0f, false);
            }

            // The body is built from a deferred queue, so report what actually landed rather than what
            // was asked for.
            const SRigidBodyComponent* FloorBody = World->TryGetComponent<SRigidBodyComponent>(FloorBodyEntity);
            if (!bFloorBodyReported && FloorBody != nullptr && FloorBody->BodyID != 0xFFFFFFFF)
            {
                bFloorBodyReported = true;
                LOG_INFO("PhysicsAsset sim: floor body {} created at {}, half extent {}", FloorBody->BodyID,
                    FloorTransform.GetLocation().y, World->GetComponent<SBoxColliderComponent>(FloorBodyEntity).HalfExtent.y);
            }
            else if (!bFloorBodyReported && SimulationFrames == 60)
            {
                LOG_WARN("PhysicsAsset sim: floor body was never created; the ragdoll has nothing to land on.");
            }
        }

        // Live bone positions, recovered from the skinning matrices the ragdoll wrote back
        // (Skin = Global * InvBind, so Global = Skin * inverse(InvBind)).
        FSkeletonResource* Resource = GetSkeletonResource();
        const SSkeletalMeshComponent* MeshComponent = (MeshEntity != entt::null)
            ? World->TryGetComponent<SSkeletalMeshComponent>(MeshEntity) : nullptr;

        if (Resource == nullptr || MeshComponent == nullptr
            || (int32)MeshComponent->BoneTransforms.size() != Resource->GetNumBones())
        {
            return;
        }

        const FMatrix4 EntityMatrix = World->GetComponent<STransformComponent>(MeshEntity).GetWorldMatrix();

        for (int32 i = 0; i < Resource->GetNumBones(); ++i)
        {
            const FMatrix4 BoneWorld = EntityMatrix * (MeshComponent->BoneTransforms[i] * Math::Inverse(Resource->GetBone(i).InvBindMatrix));
            const FVector3 Position = FVector3(BoneWorld[3]);

            const bool bHasBody = GetAsset<CPhysicsAsset>()->FindBodyIndex(Resource->GetBone(i).Name) != INDEX_NONE;
            World->DrawSphere(Position, bHasBody ? 0.03f : 0.015f,
                bHasBody ? BodyColor : FVector4(0.5f, 0.5f, 0.55f, 1.0f), 8, 2.0f, false);

            const int32 ParentIndex = Resource->GetBone(i).ParentIndex;
            if (ParentIndex != INDEX_NONE)
            {
                const FMatrix4 ParentWorld = EntityMatrix * (MeshComponent->BoneTransforms[ParentIndex] * Math::Inverse(Resource->GetBone(ParentIndex).InvBindMatrix));
                World->DrawLine(FVector3(ParentWorld[3]), Position, BoneLineColor, 1.5f, false);
            }
        }
    }

    void FPhysicsAssetEditorTool::StartSimulation()
    {
        if (bSimulating || !World.IsValid() || MeshEntity == entt::null)
        {
            return;
        }

        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();
        if (PhysicsAsset->Bodies.empty())
        {
            return;
        }

        World->EnsurePhysicsScene();

        // The visual floor plane carries no collider, so the ragdoll needs something to land on.
        FloorBodyEntity = World->ConstructEntity("Simulation Floor");
        World->GetComponent<STransformComponent>(FloorBodyEntity).SetLocation(FVector3(0.0f, -0.5f, 0.0f));

        // Configured body first, then the collider: the Jolt body is built from what the component holds
        // when it is constructed, so the values have to be passed in rather than assigned afterwards.
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

        // Leave the entity where it is: moving it to follow the root drags the preview out of frame.
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
        GrabbedBodyID = 0xFFFFFFFF;

        if (MeshEntity != entt::null)
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

        if (FloorBodyEntity != entt::null)
        {
            World->DestroyEntity(FloorBodyEntity);
            FloorBodyEntity = entt::null;
        }

        bSimulating = false;
    }

    void FPhysicsAssetEditorTool::SyncDetailsTable()
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        void* Target = nullptr;
        CStruct* Type = nullptr;

        if (SelectionMode == EPhysicsAssetSelection::Body
            && SelectedBodyIndex >= 0 && SelectedBodyIndex < (int32)PhysicsAsset->Bodies.size())
        {
            Target = &PhysicsAsset->Bodies[SelectedBodyIndex];
            Type = SPhysicsBodySetup::StaticStruct();
        }
        else if (SelectionMode == EPhysicsAssetSelection::Constraint
            && SelectedConstraintIndex >= 0 && SelectedConstraintIndex < (int32)PhysicsAsset->Constraints.size())
        {
            Target = &PhysicsAsset->Constraints[SelectedConstraintIndex];
            Type = SPhysicsConstraintSetup::StaticStruct();
        }

        // Comparing the address catches a vector reallocation as well as a selection change; the rows
        // cache a pointer into the element and would write through a dangling one after a resize.
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
            ImGui::BeginDisabled(PhysicsAsset->Bodies.empty() || MeshEntity == entt::null);
            if (ImGui::Button(LE_ICON_PLAY " Simulate"))
            {
                StartSimulation();
            }
            ImGui::EndDisabled();

            if (MeshEntity == entt::null)
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

        // Editing the body list while a ragdoll is live would leave the simulated bodies describing a
        // setup that no longer exists.
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
        ImGui::TextDisabled("%d bodies, %d constraints", (int)PhysicsAsset->Bodies.size(), (int)PhysicsAsset->Constraints.size());

        ImGui::Separator();
        ImGui::Spacing();

        BoneListView.Draw(BoneListContext);
    }

    void FPhysicsAssetEditorTool::DrawConstraintsWindow()
    {
        CPhysicsAsset* PhysicsAsset = GetAsset<CPhysicsAsset>();

        if (PhysicsAsset->Constraints.empty())
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

            int32 PendingRemoval = INDEX_NONE;

            for (int32 i = 0; i < (int32)PhysicsAsset->Constraints.size(); ++i)
            {
                const SPhysicsConstraintSetup& Constraint = PhysicsAsset->Constraints[i];

                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                const bool bSelected = (SelectionMode == EPhysicsAssetSelection::Constraint && i == SelectedConstraintIndex);

                // Empty label + AllowOverlap: a labelled row-spanning selectable draws its text across the
                // other columns and swallows the delete button's clicks.
                if (ImGui::Selectable("##Row", bSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    SelectConstraint(i);
                }

                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextUnformatted(Constraint.ParentBone.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Constraint.ChildBone.c_str());

                ImGui::TableNextColumn();
                if (ImGui::SmallButton(LE_ICON_DELETE))
                {
                    PendingRemoval = i;
                }

                ImGui::PopID();
            }

            ImGui::EndTable();

            // Deferred: erasing mid-iteration invalidates the loop and every row drawn after it.
            if (PendingRemoval != INDEX_NONE)
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

        if (DetailsTarget != nullptr)
        {
            DetailsTable->DrawTree();
        }
        else
        {
            ImGui::TextDisabled("Select a bone with a body, or a constraint.");
        }

        ImGui::Spacing();
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::SeparatorText("Physics Asset");
        ImGuiX::Font::PopFont();
        ImGui::Spacing();

        PropertyTable.DrawTree();
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
            "A swing-twist joint limiting how far a child body rotates against its parent. The cone in the "
            "viewport is the first swing limit; the line runs to the parent bone.");
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

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), CenterID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(SkeletonTreeWindowName).c_str(), LeftID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(ConstraintsWindowName).c_str(), LeftBottomID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(DetailsWindowName).c_str(), RightID);
    }
}
