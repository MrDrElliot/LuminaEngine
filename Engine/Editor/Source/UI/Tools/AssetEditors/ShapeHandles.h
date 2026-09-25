#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    class CWorld;

    /** Which dimension a draggable dot owns, shared by the collision shape and physics asset editors. */
    enum class EShapeHandle : uint8
    {
        None,
        Radius,
        HalfHeight,
        ExtentX,
        ExtentY,
        ExtentZ,
    };

    /** One draggable dot. Dragging measures along Axis from Anchor, and that distance becomes the dimension. */
    struct FShapeHandle
    {
        EShapeHandle Type = EShapeHandle::None;
        FVector3     Position;
        FVector3     Axis;
        FVector3     Anchor;
    };

    /** Drag state the shared viewport tick owns, held by whichever editor is driving it. */
    struct FShapeHandleState
    {
        EShapeHandle ActiveHandle = EShapeHandle::None;
        bool         bGizmoTransactionOpen = false;
        bool         bHandleTransactionOpen = false;
    };

    /** Everything about this frame's viewport that the shared tick reads. */
    struct FShapeHandleFrame
    {
        CWorld*             World = nullptr;
        FMatrix4            ViewMatrix = FMatrix4(1.0f);
        FMatrix4            Projection = FMatrix4(1.0f);
        ImVec2              ViewportOrigin = ImVec2(0.0f, 0.0f);
        ImVec2              ViewportSize = ImVec2(0.0f, 0.0f);
        ImGuizmo::OPERATION GizmoOp = ImGuizmo::TRANSLATE;
        bool                bViewportHovered = false;
        bool                bSuppressClicks = false;
        bool                bHasGizmoTarget = false;
    };

    /** What an editor supplies so the shared tick can gizmo, resize and pick its shapes. */
    class IShapeHandleHost
    {
    public:

        virtual ~IShapeHandleHost() = default;

        virtual FMatrix4 GetGizmoMatrix() = 0;
        virtual void     ApplyGizmoMatrix(const FMatrix4& Matrix) = 0;

        virtual void GatherShapeHandles(TVector<FShapeHandle>& OutHandles) = 0;
        virtual void ApplyShapeHandleDrag(const FShapeHandle& Handle, const FVector3& RayOrigin, const FVector3& RayDirection) = 0;
        virtual void PickShapeAtRay(const FVector3& RayOrigin, const FVector3& RayDirection) = 0;

        virtual FName GetMoveTransactionName() const = 0;
        virtual FName GetRotateTransactionName() const = 0;
        virtual FName GetResizeTransactionName() const = 0;

        virtual void BeginShapeTransaction(FName Name) = 0;
        virtual void EndShapeTransaction() = 0;
    };

    /** Gizmo, then resize dots, then click-to-pick, in that priority order. */
    void TickShapeHandles(IShapeHandleHost& Host, FShapeHandleState& State, const FShapeHandleFrame& Frame);

    namespace ShapeHandles
    {
        /** Cursor to world ray through an explicit viewport rect. False when it cannot be built. */
        bool BuildViewportRay(CWorld* World, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize,
                              const ImVec2& ScreenPos, FVector3& OutOrigin, FVector3& OutDirection);

        /** Ray against an oriented box. OutT is the world distance, so hits of different shapes sort. */
        bool RayHitsBox(const FVector3& Origin, const FVector3& Direction, const FMatrix4& Matrix,
                        const FVector3& HalfExtent, float& OutT);

        /** World position to viewport pixels. False when the point is at or behind the eye. */
        bool ProjectToScreen(const FMatrix4& ViewProj, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize,
                             const FVector3& WorldPosition, ImVec2& OutScreen);
    }
}
