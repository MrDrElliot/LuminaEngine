#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"

#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Containers/Array.h"
#include "Core/Math/Math.h"
#include "Memory/SmartPtr.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    class CSkeleton;
    struct FSkeletonResource;

    enum class EPhysicsAssetSelection : uint8
    {
        None,
        Body,
        Constraint,
    };

    enum class EPhysicsBodyHandle : uint8
    {
        None,
        Radius,
        HalfHeight,
        ExtentX,
        ExtentY,
        ExtentZ,
    };

    // One draggable dot. Position is where it draws; dragging measures along Axis from Anchor (the body
    // center) and that distance becomes the dimension the handle owns.
    struct FPhysicsHandle
    {
        EPhysicsBodyHandle  Type = EPhysicsBodyHandle::None;
        FVector3            Position;
        FVector3            Axis;
        FVector3            Anchor;
    };

    // Editor for CPhysicsAsset: skeleton tree on the left, bodies and constraint limits drawn over a
    // bind-pose preview mesh, details for whichever body/constraint is selected.
    class FPhysicsAssetEditorTool : public FAssetEditorTool
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
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

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

        void DrawBodies();
        void DrawConstraints();

        // While simulating: the physics floor and the live bone positions, so a ragdoll that misses the
        // floor is visibly a placement problem instead of a guess.
        void DrawSimulationOverlay();

        // Each of these opens its own transaction, so none may call another.
        int32 AddBodyForBone(const FName& BoneName);
        void RemoveBodyAt(int32 BodyIndex);
        int32 AddConstraintForBone(const FName& ChildBone);
        void RemoveConstraintAt(int32 ConstraintIndex);
        void ClearAllBodies();

        // Capsule down every bone long enough to matter, plus a swing-twist joint to the nearest
        // ancestor that also got a body.
        void GenerateFromSkeleton(bool bReplaceExisting);

        void SelectBody(int32 BodyIndex);
        void SelectConstraint(int32 ConstraintIndex);
        void ClearSelection();

        // Drops a live ragdoll into the preview world. The world is an editor world, so it starts paused
        // and without a physics scene; both are turned on here and put back on stop.
        void StartSimulation();
        void StopSimulation();

        // Repoints the details table when the selection changes or a vector reallocates under it.
        void SyncDetailsTable();

        FName FindAncestorBodyBone(int32 BoneIndex);

        // Viewport manipulation. Rays are built from the editor camera; picking is analytic against the
        // authored shapes rather than a GPU readback, which keeps it exact and off the render path.
        bool BuildViewportRay(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize, const ImVec2& ScreenPos,
                              FVector3& OutOrigin, FVector3& OutDirection);
        static bool ProjectToScreen(const FMatrix4& ViewProj, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize,
                                    const FVector3& WorldPosition, ImVec2& OutScreen);

        // Writes a gizmo-edited world frame back into the bone-relative offsets the asset stores.
        void ApplyBodyGizmo(const FMatrix4& NewBodyMatrix);

        // A viewport drag is one undo step: the snapshot is taken when the drag starts, not per frame,
        // so scrubbing a handle across the viewport collapses to a single entry.
        void BeginAssetTransaction(FName Name);
        void EndAssetTransaction();

        // Closes whatever a drag left open when it is cut short (entering simulation, closing the tool)
        // rather than the mouse being released.
        void CloseOpenDragTransaction();

        int32 PickBody(const FVector3& RayOrigin, const FVector3& RayDirection);
        void GatherBodyHandles(TVector<FPhysicsHandle>& OutHandles);
        void ApplyHandleDrag(const FPhysicsHandle& Handle, const FVector3& RayOrigin, const FVector3& RayDirection);

        // Mirrors a viewport pick into the bone tree so both panels agree on the selection.
        void SyncTreeSelectionToBody(int32 BodyIndex);

        // Shift + left-drag while simulating: springs the grabbed body toward the cursor, so a ragdoll can
        // be hauled around and thrown to see how the joints hold up.
        void UpdateSimulationGrab(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize);

        FTreeListViewContext        BoneListContext;
        FTreeListView               BoneListView;

        TUniquePtr<FPropertyTable>  DetailsTable;
        void*                       DetailsTarget = nullptr;
        CStruct*                    DetailsType = nullptr;

        TVector<FMatrix4>           BoneWorldTransforms;

        // Per bone: this bone or a descendant owns a body. Drives the "bodies only" tree filter.
        TVector<uint8>              BodySubtreeMask;

        entt::entity                MeshEntity = entt::null;
        entt::entity                LightEntity = entt::null;
        entt::entity                FloorBodyEntity = entt::null;

        EPhysicsAssetSelection      SelectionMode = EPhysicsAssetSelection::None;
        int32                       SelectedBodyIndex = INDEX_NONE;
        int32                       SelectedConstraintIndex = INDEX_NONE;
        FName                       SelectedBone;

        TObjectPtr<CSkeleton>       CachedSkeleton;

        float                       GenerateMinBoneLength = 0.05f;
        int32                       SimulationFrames = 0;
        EPhysicsBodyHandle          ActiveHandle = EPhysicsBodyHandle::None;

        // Size is owned by the dots, so the gizmo only ever moves or rotates the body frame.
        ImGuizmo::OPERATION         BodyGizmoOp = ImGuizmo::TRANSLATE;

        // Live grab state. Distance keeps the body at the depth it was grabbed at, so dragging sideways
        // does not haul it toward the camera; the local offset keeps the pull anchored where it was hit.
        uint32                      GrabbedBodyID = 0xFFFFFFFF;
        float                       GrabDistance = 0.0f;
        FVector3                    GrabLocalOffset = FVector3(0.0f);
        uint8                       bFloorBodyReported:1 = false;

        // Tracked separately so a gizmo drag can never close a handle drag's transaction or the reverse.
        uint8                       bHandleTransactionOpen:1 = false;
        uint8                       bGizmoTransactionOpen:1 = false;

        uint8                       bDrawBodies:1 = true;
        uint8                       bDrawConstraints:1 = true;
        uint8                       bDrawBoneLines:1 = true;
        uint8                       bBodyBonesOnly:1 = false;
        uint8                       bSimulating:1 = false;
    };
}
