#pragma once
#include "Physics/Ray/RayCast.h"

#include "World/ECS/Registry.h"


#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"

#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Memory/SmartPtr.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "UI/Tools/AssetEditors/ShapeHandles.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    class CSkeleton;
    class CSkeletalMesh;
    struct FSkeletonResource;

    enum class EPhysicsAssetSelection : uint8
    {
        None,
        Body,
        Constraint,
    };

    // Editor for CPhysicsAsset with bodies and constraint limits drawn over a bind-pose preview mesh.
    class EDITOR_API FPhysicsAssetEditorTool : public FAssetEditorTool, public IShapeHandleHost
    {
    public:

        LUMINA_EDITOR_TOOL(FPhysicsAssetEditorTool)

        FPhysicsAssetEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_SKULL; }

        void OnInitialize() override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override { CloseOpenDragTransaction(); StopSimulation(); }
        void OnAssetDataChangedExternally() override;
        void OnPostUndoRedo() override;
        void OnPropertyEditFinished(const FPropertyChangedEvent& Event) override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

        // Pull the runtime ligament gives the limb at an angle about one joint axis (0 twist, 1 swing 1, 2 swing 2), per unit of the inertia it swings.
        static float SampleSoftLimitPull(const SPhysicsConstraintSetup& Constraint, int32 Axis, float AngleRadians);

        // Copies the source constraint's soft limit settings onto the others, only those at or below its child bone when SubtreeSkeleton is given.
        static void CopySoftLimits(CPhysicsAsset& Asset, int32 SourceIndex, const FSkeletonResource* SubtreeSkeleton);

    private:

        void DrawSkeletonTreeWindow();
        void DrawConstraintsWindow();
        void DrawDetailsWindow();

        void SetupTreeContext();

        CSkeleton* GetSkeleton();
        FSkeletonResource* GetSkeletonResource();

        // Preview entity + bone tree follow the asset's Skeleton; both are rebuilt when it is reassigned.
        void RefreshForSkeletonChange();

        void RebuildBoneWorldTransforms();
        void RebuildBodySubtreeMask();

        FMatrix4 GetBodyWorldMatrix(const SPhysicsBodySetup& Body);

        bool HasBodyBone(const SPhysicsBodySetup& Body);

        void DrawBodies();
        void DrawConstraints();

        // The joint's pivot and frame as the runtime builds them, false when its child bone is missing.
        bool GetConstraintFrame(const SPhysicsConstraintSetup& Constraint, FVector3& OutPivot, FQuat& OutFrame);
        SPhysicsConstraintSetup* GetSelectedConstraint();
        float GetPreviewScale();

        void DrawSoftLimitResponse(const SPhysicsConstraintSetup& Constraint);
        void ApplySelectedSoftLimits(bool bSubtreeOnly);

        // Pushes the asset's joint limits into the running ragdoll, so tuning is felt without restarting.
        void RetuneSimulatedRagdoll();

        // Shows the physics floor and live bone positions so a ragdoll missing the floor reads as a placement problem.
        void DrawSimulationOverlay();

        // Each of these opens its own transaction, so none may call another.
        int32 AddBodyForBone(const FName& BoneName);
        void RemoveBodyAt(int32 BodyIndex);
        int32 AddConstraintForBone(const FName& ChildBone);
        void RemoveConstraintAt(int32 ConstraintIndex);
        void ClearAllBodies();

        // A capsule per bone long enough to matter, with a swing-twist joint to the nearest ancestor that has a body.
        void GenerateFromSkeleton(bool bReplaceExisting);

        void SelectBody(int32 BodyIndex);
        void SelectConstraint(int32 ConstraintIndex);
        void ClearSelection();

        // The editor world starts paused without a physics scene, so both are turned on here and restored on stop.
        void StartSimulation();
        void StopSimulation();

        // Repoints the details table when the selection changes or a vector reallocates under it.
        void SyncDetailsTable();

        FName FindAncestorBodyBone(int32 BoneIndex);

        // Viewport manipulation picks analytically against the authored shapes, off the render path.

        // Writes a gizmo-edited world frame back into the bone-relative offsets the asset stores.

        // Closes a drag cut short by entering simulation or closing the tool.
        void CloseOpenDragTransaction();

        int32 PickBody(const FVector3& RayOrigin, const FVector3& RayDirection);
        //~ IShapeHandleHost
        FMatrix4 GetGizmoMatrix() override;
        void  ApplyGizmoMatrix(const FMatrix4& NewBodyMatrix) override;
        void  GatherShapeHandles(TVector<FShapeHandle>& OutHandles) override;
        void  ApplyShapeHandleDrag(const FShapeHandle& Handle, const FVector3& RayOrigin, const FVector3& RayDirection) override;
        void  PickShapeAtRay(const FVector3& RayOrigin, const FVector3& RayDirection) override;
        FName GetMoveTransactionName() const override   { return "Move Body Frame"; }
        FName GetRotateTransactionName() const override { return "Rotate Body Frame"; }
        FName GetResizeTransactionName() const override { return SelectionMode == EPhysicsAssetSelection::Constraint ? "Edit Constraint Limit" : "Resize Body"; }
        void  BeginShapeTransaction(FName Name) override { BeginAssetTransaction(Name); }
        void  EndShapeTransaction() override             { EndAssetTransaction(); }

        // Mirrors a viewport pick into the bone tree so both panels agree on the selection.
        void SyncTreeSelectionToBody(int32 BodyIndex);

        // Shift and left-drag while simulating springs the grabbed body toward the cursor.
        void UpdateSimulationGrab(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize);

        FTreeListViewContext        BoneListContext;
        FTreeListView               BoneListView;

        TUniquePtr<FPropertyTable>  DetailsTable;
        void*                       DetailsTarget = nullptr;
        CStruct*                    DetailsType = nullptr;

        TVector<FMatrix4>           BoneWorldTransforms;

        // Per bone: this bone or a descendant owns a body. Drives the "bodies only" tree filter.
        TVector<uint8>              BodySubtreeMask;

        ECS::FEntity                MeshEntity = ECS::NullEntity;
        ECS::FEntity                LightEntity = ECS::NullEntity;
        ECS::FEntity                FloorBodyEntity = ECS::NullEntity;

        EPhysicsAssetSelection      SelectionMode = EPhysicsAssetSelection::None;
        int32                       SelectedBodyIndex = Constants::kIndexNone;
        int32                       SelectedConstraintIndex = Constants::kIndexNone;
        FName                       SelectedBone;

        TStrongObjectPtr<CSkeleton> CachedSkeleton;
        TWeakObjectPtr<CSkeletalMesh> CachedPreviewMesh;

        float                       GenerateMinBoneLength = 0.05f;
        int32                       ResponseCurveAxis = 1;
        int32                       SimulationFrames = 0;
        FShapeHandleState           HandleState;

        // Size is owned by the dots, so the gizmo only ever moves or rotates the body frame.
        ImGuizmo::OPERATION         BodyGizmoOp = ImGuizmo::TRANSLATE;

        // Distance keeps the body at its grab depth, and the local offset anchors the pull where it was hit.
        SPhysicsBodyTarget                      GrabbedTarget;
        float                       GrabDistance = 0.0f;
        FVector3                    GrabLocalOffset = FVector3(0.0f);
        uint8                       bFloorBodyReported:1 = false;

        // Tracked separately so a gizmo drag can never close a handle drag's transaction or the reverse.

        uint8                       bDrawBodies:1 = true;
        uint8                       bDrawConstraints:1 = true;
        uint8                       bDrawBoneLines:1 = true;
        uint8                       bBodyBonesOnly:1 = false;
        uint8                       bSimulating:1 = false;
    };
}
