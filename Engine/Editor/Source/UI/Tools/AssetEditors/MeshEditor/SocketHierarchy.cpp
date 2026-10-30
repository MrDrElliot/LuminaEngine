#include "EditorPCH.h"
#include "SocketHierarchy.h"

#include "Assets/AssetTypes/Mesh/MeshSocket.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Containers/Algorithm.h"
#include "Containers/StringFormat.h"
#include "Renderer/SkeletonResource.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/Transactions/EditorTransaction.h"
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"
#include "World/World.h"
#include "World/Entity/Components/CameraComponent.h"

namespace Lumina
{
    // Skeleton sockets match the viewport marker's gold, mesh sockets get their own teal so the two never read alike.
    static constexpr ImVec4 kSkeletonSocketColor     = ImVec4(1.0f, 0.82f, 0.4f, 1.0f);
    static constexpr ImVec4 kSkeletonSocketTextColor = ImVec4(0.96f, 0.91f, 0.80f, 1.0f);
    static constexpr ImVec4 kMeshSocketColor         = ImVec4(0.40f, 0.84f, 0.96f, 1.0f);
    static constexpr ImVec4 kMeshSocketTextColor     = ImVec4(0.82f, 0.94f, 0.98f, 1.0f);
    static constexpr ImVec4 kBoneIconColor           = ImVec4(0.62f, 0.67f, 0.76f, 1.0f);

    static FVector4 ToVector(const ImVec4& Color) { return FVector4(Color.x, Color.y, Color.z, Color.w); }

    void FSocketHierarchy::Initialize(CSkeleton* InSkeleton, CSkeletalMesh* InMesh, FTransactionManager* InTransactions)
    {
        Skeleton = InSkeleton;
        Mesh = InMesh;
        Transactions = InTransactions;

        TreeContext.RebuildTreeFunction = [this](FTreeListView& Tree) { RebuildTree(Tree); };
        TreeContext.ItemContextMenuFunction = [this](FTreeListView& Tree, FTreeNodeID Item) { DrawContextMenu(Tree, Item); };
        TreeContext.ItemSelectedFunction = [this](FTreeListView& Tree, FTreeNodeID Item, bool)
        {
            SelectedBoneIndex = Constants::kIndexNone;
            SelectedSocket = FSocketRef();
            if (Item.IsValid())
            {
                const FRow& Row = Tree.Get<FRow>(Item);
                if (Row.Socket.IsSet())
                {
                    SelectedSocket = Row.Socket;
                }
                else
                {
                    SelectedBoneIndex = Row.BoneIndex;
                }
            }

            if (OnBoneSelected)
            {
                OnBoneSelected(SelectedBoneIndex);
            }
        };

        TreeView.MarkTreeDirty();
    }

    void FSocketHierarchy::DrawTree()
    {
        if (Skeleton == nullptr || Skeleton->GetSkeletonResource() == nullptr)
        {
            ImGui::TextDisabled("No skeleton.");
            return;
        }

        if (Mesh != nullptr)
        {
            ImGui::TextColored(kSkeletonSocketColor, LE_ICON_AXIS_ARROW);
            ImGui::SameLine();
            ImGui::TextDisabled("Skeleton socket");
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::TextColored(kMeshSocketColor, LE_ICON_AXIS_ARROW);
            ImGui::SameLine();
            ImGui::TextDisabled("Mesh socket");
            ImGui::Spacing();
        }

        TreeView.Draw(TreeContext);
    }

    void FSocketHierarchy::OnUndoRedo()
    {
        if (Skeleton == nullptr)
        {
            return;
        }

        Skeleton->BumpSocketGeneration();
        if (Mesh != nullptr)
        {
            Mesh->BumpSocketGeneration();
        }
        if (!Find(SelectedSocket))
        {
            SelectedSocket = FSocketRef();
        }
        Refresh();
    }

    void FSocketHierarchy::RebuildTree(FTreeListView& Tree)
    {
        FSkeletonResource* Resource = Skeleton != nullptr ? Skeleton->GetSkeletonResource() : nullptr;
        if (Resource == nullptr)
        {
            return;
        }

        TVector<FSocketRef> Sockets;
        CollectVisibleSockets(Sockets);

        // The widget draws IconText over the start of the row, so the label carries spacing for it.
        auto AddRow = [&](FTreeNodeID Parent, const char* Icon, const FString& Text, const FRow& Row) -> FTreeNodeID
        {
            const FString Label = FString(Icon) + "  " + Text;
            FTreeNodeID Node = Tree.CreateNode(Parent, FStringView(Label.c_str()));
            Tree.EmplaceUserData<FRow>(Node, Row);
            FTreeNodeState& State = Tree.Get<FTreeNodeState>(Node);
            State.bExpanded = true;
            State.bSelected = Row.Socket.IsSet() ? (Row.Socket == SelectedSocket) : (Row.BoneIndex == SelectedBoneIndex);
            Tree.Get<FTreeNodeDisplay>(Node).IconText = Icon;
            return Node;
        };

        TFunction<void(FTreeNodeID, int32)> AddBone;
        AddBone = [&](FTreeNodeID Parent, int32 BoneIndex)
        {
            const FName BoneName = Resource->GetBone(BoneIndex).Name;
            FTreeNodeID BoneNode = AddRow(Parent, LE_ICON_BONE, BoneName.c_str(), FRow{ BoneIndex, FSocketRef() });
            {
                FTreeNodeDisplay& Display = Tree.Get<FTreeNodeDisplay>(BoneNode);
                Display.IconColor = kBoneIconColor;
                Display.TooltipTitle = BoneName.c_str();
                Display.TooltipSubtitle = FString("Bone ") + Format("{}", BoneIndex).c_str();
            }

            // Sockets lead their bone's children, so they stay next to the bone they hang off.
            for (const FSocketRef& Ref : Sockets)
            {
                const SMeshSocket* Socket = Find(Ref);
                if (BoneOf(*Socket) != BoneIndex)
                {
                    continue;
                }

                const bool bMeshSocket = (Ref.Owner == (CObject*)Mesh);
                const bool bOverrides = bMeshSocket && Skeleton->FindSocket(Ref.Name) != nullptr;

                FString Text(Ref.Name.c_str());
                if (bOverrides)
                {
                    Text += "  (override)";
                }

                FTreeNodeID SocketNode = AddRow(BoneNode, LE_ICON_AXIS_ARROW, Text, FRow{ BoneIndex, Ref });
                FTreeNodeDisplay& Display = Tree.Get<FTreeNodeDisplay>(SocketNode);
                Display.IconColor = bMeshSocket ? kMeshSocketColor : kSkeletonSocketColor;
                Display.DisplayColor = bMeshSocket ? kMeshSocketTextColor : kSkeletonSocketTextColor;
                Display.TooltipTitle = Ref.Name.c_str();
                Display.TooltipSubtitle = FString(bMeshSocket ? "Mesh socket on " : "Skeleton socket on ") + BoneName.c_str();
                if (bOverrides)
                {
                    Display.TooltipSubtitle += ", replacing the skeleton socket of the same name";
                }
                else if (!bMeshSocket && Mesh != nullptr)
                {
                    Display.TooltipSubtitle += ", shared by every mesh on this skeleton";
                }
            }

            for (int32 ChildIndex : Resource->GetChildBones(BoneIndex))
            {
                AddBone(BoneNode, ChildIndex);
            }
        };

        for (int32 RootIndex : Resource->GetRootBones())
        {
            AddBone(InvalidTreeNode, RootIndex);
        }
    }

    void FSocketHierarchy::DrawContextMenu(FTreeListView& Tree, FTreeNodeID Item)
    {
        if (!Item.IsValid())
        {
            return;
        }

        const FRow Row = Tree.Get<FRow>(Item);

        if (!Row.Socket.IsSet())
        {
            const FName BoneName = Skeleton->GetSkeletonResource()->GetBone(Row.BoneIndex).Name;
            auto AddSocket = [&](CObject* Owner)
            {
                BeginEdit("Add Socket", Owner);
                SMeshSocket& Socket = SocketsOf(Owner).emplace_back();
                Socket.SocketName = MakeUniqueSocketName(BoneName);
                Socket.BoneName = BoneName;
                SelectedSocket = FSocketRef{ Owner, Socket.SocketName };
                SelectedBoneIndex = Constants::kIndexNone;
                EndEdit();
            };

            if (Mesh == nullptr)
            {
                if (ImGui::MenuItem(LE_ICON_PLUS " Add Socket"))
                {
                    AddSocket(Skeleton);
                }
                return;
            }

            if (ImGui::MenuItem(LE_ICON_PLUS " Add Skeleton Socket"))
            {
                AddSocket(Skeleton);
            }
            ImGuiX::TextTooltip("Shared by every mesh that uses this skeleton.");

            if (ImGui::MenuItem(LE_ICON_PLUS " Add Mesh Socket"))
            {
                AddSocket(Mesh);
            }
            ImGuiX::TextTooltip("Only on this mesh.");
            return;
        }

        const bool bMeshSocket = (Row.Socket.Owner == (CObject*)Mesh);

        if (Mesh != nullptr && !bMeshSocket && ImGui::MenuItem(LE_ICON_CONTENT_COPY " Copy to Mesh"))
        {
            BeginEdit("Copy Socket to Mesh", Mesh);
            Mesh->Sockets.push_back(*Find(Row.Socket));
            SelectedSocket = FSocketRef{ Mesh, Row.Socket.Name };
            EndEdit();
            return;
        }

        if (bMeshSocket && ImGui::MenuItem(LE_ICON_ARROW_UP " Promote to Skeleton"))
        {
            BeginEdit("Promote Socket to Skeleton", Skeleton, Mesh);
            const SMeshSocket Promoted = *Find(Row.Socket);
            if (SMeshSocket* Existing = Find(FSocketRef{ Skeleton, Row.Socket.Name }))
            {
                *Existing = Promoted;
            }
            else
            {
                Skeleton->Sockets.push_back(Promoted);
            }
            Mesh->Sockets.erase(Algo::RemoveIf(Mesh->Sockets, [&](const SMeshSocket& Socket) { return Socket.SocketName == Row.Socket.Name; }), Mesh->Sockets.end());
            SelectedSocket = FSocketRef{ Skeleton, Row.Socket.Name };
            EndEdit();
            return;
        }

        if (ImGui::MenuItem(LE_ICON_DELETE " Delete Socket"))
        {
            BeginEdit("Delete Socket", Row.Socket.Owner);
            TVector<SMeshSocket>& Sockets = SocketsOf(Row.Socket.Owner);
            Sockets.erase(Algo::RemoveIf(Sockets, [&](const SMeshSocket& Socket) { return Socket.SocketName == Row.Socket.Name; }), Sockets.end());
            SelectedSocket = FSocketRef();
            EndEdit();
        }
    }

    void FSocketHierarchy::CollectVisibleSockets(TVector<FSocketRef>& Out) const
    {
        Out.clear();
        if (Skeleton == nullptr)
        {
            return;
        }

        if (Mesh != nullptr)
        {
            for (const SMeshSocket& Socket : Mesh->Sockets)
            {
                if (BoneOf(Socket) != Constants::kIndexNone)
                {
                    Out.push_back(FSocketRef{ Mesh, Socket.SocketName });
                }
            }
        }

        for (const SMeshSocket& Socket : Skeleton->Sockets)
        {
            const bool bShadowed = Mesh != nullptr && FindSocketByName(Mesh->Sockets, Socket.SocketName) != nullptr;
            if (!bShadowed && BoneOf(Socket) != Constants::kIndexNone)
            {
                Out.push_back(FSocketRef{ Skeleton, Socket.SocketName });
            }
        }
    }

    TVector<SMeshSocket>& FSocketHierarchy::SocketsOf(CObject* Owner) const
    {
        return (Mesh != nullptr && Owner == (CObject*)Mesh) ? Mesh->Sockets : Skeleton->Sockets;
    }

    SMeshSocket* FSocketHierarchy::Find(const FSocketRef& Ref) const
    {
        if (!Ref.IsSet())
        {
            return nullptr;
        }
        for (SMeshSocket& Socket : SocketsOf(Ref.Owner))
        {
            if (Socket.SocketName == Ref.Name)
            {
                return &Socket;
            }
        }
        return nullptr;
    }

    int32 FSocketHierarchy::BoneOf(const SMeshSocket& Socket) const
    {
        const FSkeletonResource* Resource = Skeleton->GetSkeletonResource();
        return Resource != nullptr ? Resource->FindBoneIndex(Socket.BoneName) : Constants::kIndexNone;
    }

    FMatrix4 FSocketHierarchy::SocketWorldMatrix(const SMeshSocket& Socket) const
    {
        const int32 BoneIndex = BoneOf(Socket);
        if (BoneIndex < 0 || BoneIndex >= (int32)BoneWorldTransforms.size())
        {
            return Socket.RelativeTransform.GetMatrix();
        }
        return BoneWorldTransforms[BoneIndex] * Socket.RelativeTransform.GetMatrix();
    }

    FName FSocketHierarchy::MakeUniqueSocketName(FName BoneName) const
    {
        auto IsTaken = [&](FName Name)
        {
            return Skeleton->FindSocket(Name) != nullptr || (Mesh != nullptr && FindSocketByName(Mesh->Sockets, Name) != nullptr);
        };

        const FString Base = FString(BoneName.c_str()) + "_Socket";
        FName Name(Base.c_str());
        for (int32 Suffix = 1; IsTaken(Name); ++Suffix)
        {
            Name = FName((Base + "_" + Format("{}", Suffix).c_str()).c_str());
        }
        return Name;
    }

    void FSocketHierarchy::BeginEdit(FName Label, CObject* First, CObject* Second)
    {
        EditOwners[0] = First;
        EditOwners[1] = Second;
        Transactions->BeginTransaction(Label);
        Transactions->Record(MakeUnique<FObjectSnapshotCommand>(First, Label));
        if (Second != nullptr)
        {
            Transactions->Record(MakeUnique<FObjectSnapshotCommand>(Second, Label));
        }
    }

    void FSocketHierarchy::EndEdit()
    {
        Transactions->CommitTransaction();

        // An add, delete, copy or promote can change which bones carry sockets, which bone LOD keys on.
        for (CObject* Owner : EditOwners)
        {
            if (Owner == nullptr)
            {
                continue;
            }
            MarkEdited(Owner);
            if (Owner == (CObject*)Mesh)
            {
                Mesh->BumpSocketGeneration();
            }
            else
            {
                Skeleton->BumpSocketGeneration();
            }
        }
        Refresh();
    }

    void FSocketHierarchy::MarkEdited(CObject* Owner)
    {
        if (CPackage* Package = Owner->GetPackage())
        {
            Package->MarkDirty();
        }
        if (OnSocketsEdited)
        {
            OnSocketsEdited();
        }
    }

    void FSocketHierarchy::DrawSocketMarkers(CWorld* World, float MarkerRadius)
    {
        PickRadius = MarkerRadius * 1.8f;

        TVector<FSocketRef> Sockets;
        CollectVisibleSockets(Sockets);

        for (const FSocketRef& Ref : Sockets)
        {
            const FMatrix4 Matrix = SocketWorldMatrix(*Find(Ref));
            const FVector3 Position = FVector3(Matrix[3]);

            // The selected socket carries the gizmo, which already shows its axes.
            if (Ref == SelectedSocket)
            {
                World->GetDebugInterface()->DrawSphere(Position, MarkerRadius * 1.35f, FVector4(1.0f), 12, 2.5f, false);
                continue;
            }

            const ImVec4 Color = (Ref.Owner == (CObject*)Mesh) ? kMeshSocketColor : kSkeletonSocketColor;
            const float AxisLength = MarkerRadius * 4.0f;
            World->GetDebugInterface()->DrawSphere(Position, MarkerRadius, ToVector(Color), 12, 2.5f, false);
            World->GetDebugInterface()->DrawLine(Position, Position + Math::Normalize(FVector3(Matrix[0])) * AxisLength, FVector4(1.0f, 0.2f, 0.2f, 1.0f), 3.0f, false);
            World->GetDebugInterface()->DrawLine(Position, Position + Math::Normalize(FVector3(Matrix[1])) * AxisLength, FVector4(0.2f, 1.0f, 0.2f, 1.0f), 3.0f, false);
            World->GetDebugInterface()->DrawLine(Position, Position + Math::Normalize(FVector3(Matrix[2])) * AxisLength, FVector4(0.2f, 0.4f, 1.0f, 1.0f), 3.0f, false);
        }
    }

    void FSocketHierarchy::TickViewport(CWorld* World, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize, ImGuizmo::OPERATION& GizmoOp,
                                        bool bViewportHovered, bool bSuppressClicks, float MarkerRadius)
    {
        PickRadius = MarkerRadius * 1.8f;
        SCameraComponent* Camera = World != nullptr ? World->GetCamera()->GetActiveCamera() : nullptr;
        if (Camera == nullptr || BoneWorldTransforms.empty())
        {
            return;
        }

        if (bViewportHovered && ImGui::IsKeyPressed(ImGuiKey_Space))
        {
            GizmoOp = (GizmoOp == ImGuizmo::TRANSLATE) ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE;
        }

        FMatrix4 Projection = Camera->GetProjectionMatrix();
        Projection[1][1] *= -1.0f;

        FShapeHandleFrame Frame;
        Frame.World            = World;
        Frame.ViewMatrix       = Camera->GetViewMatrix();
        Frame.Projection       = Projection;
        Frame.ViewportOrigin   = ViewportOrigin;
        Frame.ViewportSize     = ViewportSize;
        Frame.GizmoOp          = GizmoOp;
        Frame.bViewportHovered = bViewportHovered;
        Frame.bSuppressClicks  = bSuppressClicks;
        Frame.bHasGizmoTarget  = Find(SelectedSocket) != nullptr;

        TickShapeHandles(*this, HandleState, Frame);
    }

    FMatrix4 FSocketHierarchy::GetGizmoMatrix()
    {
        return SocketWorldMatrix(*Find(SelectedSocket));
    }

    void FSocketHierarchy::ApplyGizmoMatrix(const FMatrix4& Matrix)
    {
        SMeshSocket* Socket = Find(SelectedSocket);
        const int32 BoneIndex = Socket != nullptr ? BoneOf(*Socket) : Constants::kIndexNone;
        if (BoneIndex < 0 || BoneIndex >= (int32)BoneWorldTransforms.size())
        {
            return;
        }

        Socket->RelativeTransform = FTransform(Math::Inverse(BoneWorldTransforms[BoneIndex]) * Matrix);
        MarkEdited(SelectedSocket.Owner);
    }

    void FSocketHierarchy::BeginShapeTransaction(FName Name)
    {
        BeginEdit(Name, SelectedSocket.Owner);
    }

    void FSocketHierarchy::CloseOpenDragTransaction()
    {
        if (HandleState.bHandleTransactionOpen || HandleState.bGizmoTransactionOpen)
        {
            HandleState.bHandleTransactionOpen = false;
            HandleState.bGizmoTransactionOpen = false;
            if (Transactions != nullptr)
            {
                Transactions->CommitTransaction();
            }
        }
    }

    void FSocketHierarchy::EndShapeTransaction()
    {
        // Only the offset moved, so the bones carrying sockets and both generations stay as they were.
        Transactions->CommitTransaction();
    }

    void FSocketHierarchy::PickShapeAtRay(const FVector3& RayOrigin, const FVector3& RayDirection)
    {
        TVector<FSocketRef> Sockets;
        CollectVisibleSockets(Sockets);

        FSocketRef Closest;
        float ClosestT = FLT_MAX;
        for (const FSocketRef& Ref : Sockets)
        {
            const FVector3 Position = FVector3(SocketWorldMatrix(*Find(Ref))[3]);
            const float T = Math::Dot(Position - RayOrigin, RayDirection);
            if (T > 0.0f && T < ClosestT && Math::Distance(RayOrigin + RayDirection * T, Position) <= PickRadius)
            {
                ClosestT = T;
                Closest = Ref;
            }
        }

        if (Closest == SelectedSocket)
        {
            return;
        }

        SelectedSocket = Closest;
        if (Closest.IsSet())
        {
            SelectedBoneIndex = Constants::kIndexNone;
        }
        Refresh();
    }
}
