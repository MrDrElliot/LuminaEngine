#include "EditorPCH.h"
#include "ShapeHandles.h"

#include "World/World.h"
#include "World/Entity/Components/CameraComponent.h"

#include <algorithm>

namespace Lumina
{
    void TickShapeHandles(IShapeHandleHost& Host, FShapeHandleState& State, const FShapeHandleFrame& Frame)
    {
        const FMatrix4 ViewProj = Frame.Projection * Frame.ViewMatrix;

        // Frame gizmo first, so it owns the cursor before the resize dots or picking see it.
        bool bGizmoOwnsInput = false;
        if (Frame.bHasGizmoTarget)
        {
            ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
            ImGuizmo::SetRect(Frame.ViewportOrigin.x, Frame.ViewportOrigin.y, Frame.ViewportSize.x, Frame.ViewportSize.y);

            FMatrix4 Matrix = Host.GetGizmoMatrix();

            const bool bGizmoInert = Frame.bSuppressClicks && !ImGuizmo::IsUsing();
            if (bGizmoInert)
            {
                ImGuizmo::Enable(false);
            }

            ImGuizmo::Manipulate(Math::ValuePtr(Frame.ViewMatrix), Math::ValuePtr(Frame.Projection),
                Frame.GizmoOp, ImGuizmo::LOCAL, Math::ValuePtr(Matrix));

            if (bGizmoInert)
            {
                ImGuizmo::Enable(true);
            }

            if (ImGuizmo::IsUsing())
            {
                if (!State.bGizmoTransactionOpen)
                {
                    Host.BeginShapeTransaction(Frame.GizmoOp == ImGuizmo::ROTATE
                        ? Host.GetRotateTransactionName() : Host.GetMoveTransactionName());
                    State.bGizmoTransactionOpen = true;
                }

                Host.ApplyGizmoMatrix(Matrix);
            }
            else if (State.bGizmoTransactionOpen)
            {
                State.bGizmoTransactionOpen = false;
                Host.EndShapeTransaction();
            }

            bGizmoOwnsInput = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
        }

        if (bGizmoOwnsInput && State.ActiveHandle == EShapeHandle::None)
        {
            return;
        }

        TVector<FShapeHandle> Handles;
        Host.GatherShapeHandles(Handles);

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const ImVec2 MousePos = ImGui::GetMousePos();

        constexpr float HandleRadius = 6.0f;
        constexpr float HandleGrabRadius = 10.0f;

        int32 HoveredHandle = INDEX_NONE;

        for (int32 i = 0; i < (int32)Handles.size(); ++i)
        {
            ImVec2 Screen;
            if (!ShapeHandles::ProjectToScreen(ViewProj, Frame.ViewportOrigin, Frame.ViewportSize, Handles[i].Position, Screen))
            {
                continue;
            }

            const float DX = MousePos.x - Screen.x;
            const float DY = MousePos.y - Screen.y;
            const bool bHot = (State.ActiveHandle == Handles[i].Type)
                           || (State.ActiveHandle == EShapeHandle::None && (DX * DX + DY * DY) <= HandleGrabRadius * HandleGrabRadius);

            if (bHot && State.ActiveHandle == EShapeHandle::None)
            {
                HoveredHandle = i;
            }

            DrawList->AddCircleFilled(Screen, bHot ? HandleRadius + 1.5f : HandleRadius,
                bHot ? IM_COL32(255, 200, 60, 255) : IM_COL32(90, 180, 255, 235));
            DrawList->AddCircle(Screen, bHot ? HandleRadius + 1.5f : HandleRadius, IM_COL32(15, 15, 20, 220), 0, 1.5f);
        }

        const bool bCanInteract = Frame.bViewportHovered && !Frame.bSuppressClicks;

        if (State.ActiveHandle != EShapeHandle::None)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                State.ActiveHandle = EShapeHandle::None;
                if (State.bHandleTransactionOpen)
                {
                    State.bHandleTransactionOpen = false;
                    Host.EndShapeTransaction();
                }
            }
            else
            {
                for (const FShapeHandle& Handle : Handles)
                {
                    if (Handle.Type != State.ActiveHandle)
                    {
                        continue;
                    }

                    FVector3 RayOrigin, RayDirection;
                    if (ShapeHandles::BuildViewportRay(Frame.World, Frame.ViewportOrigin, Frame.ViewportSize, MousePos, RayOrigin, RayDirection))
                    {
                        Host.ApplyShapeHandleDrag(Handle, RayOrigin, RayDirection);
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
            State.ActiveHandle = Handles[HoveredHandle].Type;
            Host.BeginShapeTransaction(Host.GetResizeTransactionName());
            State.bHandleTransactionOpen = true;
            return;
        }

        // Picks on release with a drag threshold, so a click that became a camera move does not reselect.
        if (HoveredHandle == INDEX_NONE && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            const ImVec2 Drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (Drag.x * Drag.x + Drag.y * Drag.y <= 16.0f)
            {
                FVector3 RayOrigin, RayDirection;
                if (ShapeHandles::BuildViewportRay(Frame.World, Frame.ViewportOrigin, Frame.ViewportSize, MousePos, RayOrigin, RayDirection))
                {
                    Host.PickShapeAtRay(RayOrigin, RayDirection);
                }
            }
        }
    }
}

namespace Lumina::ShapeHandles
{
    bool BuildViewportRay(CWorld* World, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize,
                          const ImVec2& ScreenPos, FVector3& OutOrigin, FVector3& OutDirection)
    {
        SCameraComponent* Camera = (World != nullptr) ? World->GetActiveCamera() : nullptr;
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

    bool RayHitsBox(const FVector3& Origin, const FVector3& Direction, const FMatrix4& Matrix,
                    const FVector3& HalfExtent, float& OutT)
    {
        const FMatrix4 Inverse = Math::Inverse(Matrix);
        const FVector3 LocalOrigin = FVector3(Inverse * FVector4(Origin, 1.0f));
        const FVector3 LocalDirection = FVector3(Inverse * FVector4(Direction, 0.0f));

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
                std::swap(TNear, TFar);
            }

            TMin = Math::Max(TMin, TNear);
            TMax = Math::Min(TMax, TFar);
            if (TMin > TMax)
            {
                return false;
            }
        }

        const FVector3 WorldHit = FVector3(Matrix * FVector4(LocalOrigin + LocalDirection * TMin, 1.0f));
        OutT = Math::Length(WorldHit - Origin);
        return true;
    }

    bool ProjectToScreen(const FMatrix4& ViewProj, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize,
                         const FVector3& WorldPosition, ImVec2& OutScreen)
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
}
