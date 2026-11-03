#pragma once

#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/Tools/AssetEditors/ShapeHandles.h"

namespace Lumina
{
    class CObject;
    class CSkeleton;
    class CSkeletalMesh;
    class CWorld;
    class FTransactionManager;
    struct SMeshSocket;

    // The bone tree with sockets under their bones, plus socket markers, gizmo and picking, shared by the skeleton and skeletal mesh editors.
    class FSocketHierarchy : public IShapeHandleHost
    {
    public:

        // Mesh is null in the skeleton editor, where only skeleton sockets exist.
        void Initialize(CSkeleton* InSkeleton, CSkeletalMesh* InMesh, FTransactionManager* InTransactions);

        void DrawTree();
        void Refresh() { TreeView.MarkTreeDirty(); }

        // Undo can add, remove or rebind sockets, which the bone LOD layouts key on.
        void OnUndoRedo();

        void CloseOpenDragTransaction();

        // World space bone matrices of the pose being shown, which the markers, gizmo and picking ride.
        void SetBoneWorldTransforms(const TVector<FMatrix4>& Transforms) { BoneWorldTransforms = Transforms; }

        void DrawSocketMarkers(CWorld* World, float MarkerRadius);

        // Gizmos the selected socket, otherwise lets a click on a marker select one. Space toggles move and rotate.
        void TickViewport(CWorld* World, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize, ImGuizmo::OPERATION& GizmoOp,
                          bool bViewportHovered, bool bSuppressClicks, float MarkerRadius);

        int32 GetSelectedBoneIndex() const { return SelectedBoneIndex; }

        TFunction<void()>      OnSocketsEdited;
        TFunction<void(int32)> OnBoneSelected;

        //~ IShapeHandleHost
        FMatrix4 GetGizmoMatrix() override;
        void  ApplyGizmoMatrix(const FMatrix4& Matrix) override;
        void  GatherShapeHandles(TVector<FShapeHandle>& OutHandles) override { }
        void  ApplyShapeHandleDrag(const FShapeHandle& Handle, const FVector3& RayOrigin, const FVector3& RayDirection) override { }
        void  PickShapeAtRay(const FVector3& RayOrigin, const FVector3& RayDirection) override;
        FName GetMoveTransactionName() const override   { return "Move Socket"; }
        FName GetRotateTransactionName() const override { return "Rotate Socket"; }
        FName GetResizeTransactionName() const override { return "Scale Socket"; }
        void  BeginShapeTransaction(FName Name) override;
        void  EndShapeTransaction() override;

    private:

        // A mesh socket may share a skeleton socket's name, so a socket is named by the asset that owns it too.
        struct FSocketRef
        {
            CObject* Owner = nullptr;
            FName    Name;

            bool IsSet() const { return Owner != nullptr; }
            bool operator==(const FSocketRef& Other) const { return Owner == Other.Owner && Name == Other.Name; }
        };

        struct FRow
        {
            int32      BoneIndex = Constants::kIndexNone;
            FSocketRef Socket;
        };

        void RebuildTree(FTreeListView& Tree);
        void DrawContextMenu(FTreeListView& Tree, FTreeNodeID Item);

        // Every socket that resolves to a bone, each once, mesh sockets shadowing skeleton ones of the same name.
        void CollectVisibleSockets(TVector<FSocketRef>& Out) const;

        TVector<SMeshSocket>& SocketsOf(CObject* Owner) const;
        SMeshSocket* Find(const FSocketRef& Ref) const;
        int32 BoneOf(const SMeshSocket& Socket) const;
        FMatrix4 SocketWorldMatrix(const SMeshSocket& Socket) const;
        FName MakeUniqueSocketName(FName BoneName) const;

        void BeginEdit(FName Label, CObject* First, CObject* Second = nullptr);
        void EndEdit();
        void MarkEdited(CObject* Owner);

        // The assets the open edit snapshotted, the only ones it marks dirty when it ends.
        CObject*             EditOwners[2] = {};

        CSkeleton*           Skeleton = nullptr;
        CSkeletalMesh*       Mesh = nullptr;
        FTransactionManager* Transactions = nullptr;

        FTreeListViewContext TreeContext;
        FTreeListView        TreeView;

        TVector<FMatrix4>    BoneWorldTransforms;
        FShapeHandleState    HandleState;
        float                PickRadius = 0.08f;

        int32                SelectedBoneIndex = Constants::kIndexNone;
        FSocketRef           SelectedSocket;
    };
}
