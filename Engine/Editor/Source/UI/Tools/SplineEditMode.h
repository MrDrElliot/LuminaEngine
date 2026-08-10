#pragma once

#define USE_IMGUI_API
#include <imgui.h>

#include "WorldEditorMode.h"
#include "World/Entity/EntityHandle.h"

namespace Lumina
{
    class CWorld;
    struct SCameraComponent;
    struct SSplineComponent;

    /**
     * Spline point editing. Owns viewport input while active: click a handle to select it, drag the gizmo
     * to move it. The host's own selection/marquee/gizmo are suppressed, so clicking a spline point can
     * never be mistaken for picking a different entity.
     *
     * Operates on the selected entity that carries an SSplineComponent.
     */
    class FSplineEditMode final : public IWorldEditorMode
    {
    public:

        const char* GetDisplayName() const override { return "Spline"; }
        const char* GetIcon() const override { return LE_ICON_VECTOR_CURVE; }
        const char* GetTooltip() const override
        {
            return "Spline: click a point or tangent handle to select it, drag the gizmo to move it.\n"
                   "Select an entity with a Spline component first.";
        }

        void OnEnter(CWorld* World) override;
        void OnExit(CWorld* World) override;

        void Tick(CWorld* World,
                  const SCameraComponent& Camera,
                  bool bViewportHovered,
                  ImVec2 ViewportScreenOrigin,
                  ImVec2 ViewportSize) override;

        void DrawOverlay(CWorld* World, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera) override;
        void DrawToolbar(CWorld* World, float ButtonSize) override;

        bool ConsumesViewportInput() const override { return true; }

    private:

        /** Which part of a control point the gizmo is driving. */
        enum class EHandle : uint8
        {
            Point,
            ArriveTangent,
            LeaveTangent,
        };

        /** The selected entity that has a spline, or entt::null. */
        entt::entity FindSplineEntity(CWorld* World) const;

        /** World-space position of a handle. */
        FVector3 GetHandleWorldPosition(const SSplineComponent& Spline, const FMatrix4& LocalToWorld, int32 PointIndex, EHandle Handle) const;

        /** Write a dragged world position back into the component, in local space. */
        void ApplyHandleWorldPosition(SSplineComponent& Spline, const FMatrix4& WorldToLocal, int32 PointIndex, EHandle Handle, const FVector3& WorldPos) const;

        /** Screen-space pick against every point and visible tangent handle. Returns false if nothing was hit. */
        bool PickHandle(const SSplineComponent& Spline,
                        const FMatrix4& LocalToWorld,
                        const FMatrix4& ViewProjection,
                        ImVec2 ViewportSize,
                        ImVec2 MouseInViewport,
                        int32& OutPointIndex,
                        EHandle& OutHandle) const;

        entt::entity ActiveEntity      = entt::null;
        int32        SelectedPoint     = INDEX_NONE;
        EHandle      SelectedHandle    = EHandle::Point;

        /** Mirrors the host's bImGuizmoUsedOnce: open the transaction on the first frame of a drag only. */
        bool         bGizmoUsedOnce    = false;

        /** Draw the point index beside each handle. */
        bool         bShowPointIndices = true;
    };
}
