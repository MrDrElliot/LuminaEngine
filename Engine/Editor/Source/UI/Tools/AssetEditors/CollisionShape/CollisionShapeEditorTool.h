#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"

#include "Assets/AssetTypes/Physics/CollisionShape.h"
#include "Containers/Array.h"
#include "Core/Math/Math.h"
#include "Memory/SmartPtr.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    class CStaticMesh;

    enum class ECollisionHandle : uint8
    {
        None,
        Radius,
        HalfHeight,
        ExtentX,
        ExtentY,
        ExtentZ,
    };

    // One draggable dot. Dragging measures along Axis from Anchor, and that distance becomes the
    // dimension the handle owns.
    struct FCollisionHandle
    {
        ECollisionHandle Type = ECollisionHandle::None;
        FVector3         Position;
        FVector3         Axis;
        FVector3         Anchor;
    };

    // Editor for CCollisionShape: the source mesh in the viewport with its authored collision drawn over
    // it, so how badly a hull or box fits is visible rather than inferred from numbers.
    class FCollisionShapeEditorTool : public FAssetEditorTool
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

        // Viewport manipulation, mirroring the physics asset editor: analytic picking against the authored
        // shapes, dots for size, gizmo for the frame.
        bool BuildViewportRay(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize, const ImVec2& ScreenPos,
                              FVector3& OutOrigin, FVector3& OutDirection);
        static bool ProjectToScreen(const FMatrix4& ViewProj, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize,
                                    const FVector3& WorldPosition, ImVec2& OutScreen);

        int32 PickPrimitive(const FVector3& RayOrigin, const FVector3& RayDirection);
        void GatherHandles(TVector<FCollisionHandle>& OutHandles);
        void ApplyHandleDrag(const FCollisionHandle& Handle, const FVector3& RayOrigin, const FVector3& RayDirection);
        void ApplyGizmo(const FMatrix4& NewMatrix);

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

        entt::entity                MeshEntity = entt::null;
        entt::entity                LightEntity = entt::null;

        int32                       SelectedPrimitive = INDEX_NONE;
        ECollisionHandle            ActiveHandle = ECollisionHandle::None;
        ImGuizmo::OPERATION         GizmoOp = ImGuizmo::TRANSLATE;

        TObjectPtr<CStaticMesh>     CachedSourceMesh;

        uint8                       bDrawShapes:1 = true;
        uint8                       bDrawMesh:1 = true;
        uint8                       bHandleTransactionOpen:1 = false;
        uint8                       bGizmoTransactionOpen:1 = false;
    };
}
