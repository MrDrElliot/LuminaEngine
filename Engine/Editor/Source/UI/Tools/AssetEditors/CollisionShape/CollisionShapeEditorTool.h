#pragma once

#include "World/ECS/Registry.h"


#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"

#include "Assets/AssetTypes/Physics/CollisionShape.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Memory/SmartPtr.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "UI/Tools/AssetEditors/ShapeHandles.h"

namespace Lumina
{
    class CStaticMesh;

    // Editor for CCollisionShape: the source mesh in the viewport with its authored collision drawn over
    // it, so how badly a hull or box fits is visible rather than inferred from numbers.
    class FCollisionShapeEditorTool : public FAssetEditorTool, public IShapeHandleHost
    {
    public:

        LUMINA_EDITOR_TOOL(FCollisionShapeEditorTool)

        FCollisionShapeEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_CUBE_OUTLINE; }

        void OnInitialize() override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override { CloseOpenDragTransaction(); }
        void OnAssetDataChangedExternally() override;
        void OnPostUndoRedo() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        void DrawShapeListWindow();
        void DrawDetailsWindow();

        void RefreshPreviewMesh();

        // Hull wireframes are rebuilt from the points rather than serialized, so an edited hull can never
        // be drawn against a stale outline.
        void RebuildHullWireframes();

        void DrawPrimitives();

        FMatrix4 GetPrimitiveMatrix(const SCollisionPrimitive& Primitive) const;

        int32 AddPrimitive(ECollisionPrimitiveType Type);
        void RemovePrimitiveAt(int32 Index);
        void RunGenerator(int32 GeneratorIndex);

        void SelectPrimitive(int32 Index);
        void SyncDetailsTable();

        int32 PickPrimitive(const FVector3& RayOrigin, const FVector3& RayDirection);

        //~ IShapeHandleHost
        FMatrix4 GetGizmoMatrix() override;
        void  ApplyGizmoMatrix(const FMatrix4& Matrix) override;
        void  GatherShapeHandles(TVector<FShapeHandle>& OutHandles) override;
        void  ApplyShapeHandleDrag(const FShapeHandle& Handle, const FVector3& RayOrigin, const FVector3& RayDirection) override;
        void  PickShapeAtRay(const FVector3& RayOrigin, const FVector3& RayDirection) override;
        FName GetMoveTransactionName() const override   { return "Move Collision Shape"; }
        FName GetRotateTransactionName() const override { return "Rotate Collision Shape"; }
        FName GetResizeTransactionName() const override { return "Resize Collision Shape"; }
        void  BeginShapeTransaction(FName Name) override { BeginAssetTransaction(Name); }
        void  EndShapeTransaction() override             { EndAssetTransaction(); }

        void BeginAssetTransaction(FName Name);
        void EndAssetTransaction();
        void CloseOpenDragTransaction();

        TUniquePtr<FPropertyTable>  DetailsTable;
        void*                       DetailsTarget = nullptr;

        // Parallel to Primitives; a hull's entry is empty for non-hull types.
        struct FHullWireframe
        {
            TVector<FVector3> Vertices;
            TVector<uint32>   Edges;
        };
        TVector<FHullWireframe>     HullWireframes;

        ECS::FEntity                MeshEntity = ECS::NullEntity;
        ECS::FEntity                LightEntity = ECS::NullEntity;

        int32                       SelectedPrimitive = INDEX_NONE;
        FShapeHandleState           HandleState;
        ImGuizmo::OPERATION         GizmoOp = ImGuizmo::TRANSLATE;

        TObjectPtr<CStaticMesh>     CachedSourceMesh;

        uint8                       bDrawShapes:1 = true;
        uint8                       bDrawMesh:1 = true;
    };
}
