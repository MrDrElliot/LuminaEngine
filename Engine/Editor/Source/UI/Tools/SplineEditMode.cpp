#include "EditorPCH.h"
#include "SplineEditMode.h"

#include "ImGuizmo.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/SplineComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        const FVector4 kArriveColor  (0.35f, 0.75f, 1.00f, 1.0f);
        const FVector4 kLeaveColor   (0.35f, 1.00f, 0.55f, 1.0f);

        /** Screen-space pick radius, in pixels, for a point or tangent handle. */
        constexpr float kPickRadiusPx = 14.0f;

        /** Hermite tangents are 3x the chord they produce, so handles are drawn (and picked) at a third. */
        constexpr float kTangentHandleScale = 1.0f / 3.0f;

        /** Project a world position to viewport pixels. Returns false when it is behind the camera. */
        bool ProjectToScreen(const FMatrix4& ViewProj, const FVector3& World, ImVec2 ViewportSize, ImVec2& OutScreen)
        {
            const FVector4 Clip = ViewProj * FVector4(World, 1.0f);
            if (Clip.w <= 1.0e-4f)
            {
                return false;
            }

            OutScreen.x = (Clip.x / Clip.w * 0.5f + 0.5f) * ViewportSize.x;
            OutScreen.y = (1.0f - (Clip.y / Clip.w * 0.5f + 0.5f)) * ViewportSize.y;
            return true;
        }

        float ScreenDistSq(ImVec2 A, ImVec2 B)
        {
            const float dx = A.x - B.x;
            const float dy = A.y - B.y;
            return dx * dx + dy * dy;
        }

        void HoverTooltip(const char* Text)
        {
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", Text);
            }
        }
    }

    void FSplineEditMode::OnEnter(CWorld*)
    {
        ActiveEntity   = entt::null;
        SelectedPoint  = INDEX_NONE;
        SelectedHandle = EHandle::Point;
        bGizmoUsedOnce = false;
    }

    void FSplineEditMode::OnExit(CWorld*)
    {
        // A mode switch mid-drag would otherwise strand the open transaction.
        if (bGizmoUsedOnce && Context)
        {
            Context->EndModeTransaction("Edit Spline");
        }

        ActiveEntity   = entt::null;
        SelectedPoint  = INDEX_NONE;
        bGizmoUsedOnce = false;
    }

    entt::entity FSplineEditMode::FindSplineEntity(CWorld* World) const
    {
        if (World == nullptr)
        {
            return entt::null;
        }

        auto View = World->View<FSelectedInEditorComponent, SSplineComponent>();
        for (entt::entity Entity : View)
        {
            return Entity;
        }

        return entt::null;
    }

    FVector3 FSplineEditMode::GetHandleWorldPosition(const SSplineComponent& Spline, const FMatrix4& LocalToWorld, int32 PointIndex, EHandle Handle) const
    {
        const SSplinePoint& Point = Spline.Points[PointIndex];

        FVector3 Local = Point.Location;
        if (Handle == EHandle::ArriveTangent)
        {
            Local = Point.Location - Point.ArriveTangent * kTangentHandleScale;
        }
        else if (Handle == EHandle::LeaveTangent)
        {
            Local = Point.Location + Point.LeaveTangent * kTangentHandleScale;
        }

        return FVector3(LocalToWorld * FVector4(Local, 1.0f));
    }

    void FSplineEditMode::ApplyHandleWorldPosition(SSplineComponent& Spline, const FMatrix4& WorldToLocal, int32 PointIndex, EHandle Handle, const FVector3& WorldPos) const
    {
        SSplinePoint& Point = Spline.Points[PointIndex];
        const FVector3 Local = FVector3(WorldToLocal * FVector4(WorldPos, 1.0f));

        switch (Handle)
        {
        case EHandle::Point:
            Point.Location = Local;
            break;

        case EHandle::ArriveTangent:
            // Dragging a tangent is an explicit authoring act, so the point flips to User -- otherwise
            // UpdateTangents would overwrite the drag on the very next frame.
            Point.TangentMode  = ESplineTangentMode::User;
            Point.ArriveTangent = (Point.Location - Local) / kTangentHandleScale;
            break;

        case EHandle::LeaveTangent:
            Point.TangentMode  = ESplineTangentMode::User;
            Point.LeaveTangent = (Local - Point.Location) / kTangentHandleScale;
            break;
        }
    }

    bool FSplineEditMode::PickHandle(const SSplineComponent& Spline,
                                     const FMatrix4& LocalToWorld,
                                     const FMatrix4& ViewProjection,
                                     ImVec2 ViewportSize,
                                     ImVec2 MouseInViewport,
                                     int32& OutPointIndex,
                                     EHandle& OutHandle) const
    {
        float BestDistSq = kPickRadiusPx * kPickRadiusPx;
        bool  bFound     = false;

        for (int32 i = 0; i < static_cast<int32>(Spline.Points.size()); ++i)
        {
            // Tangent handles are only pickable on the selected point: every point showing two extra
            // handles turns a dense spline into an unpickable cloud.
            const bool bTangentsPickable = (i == SelectedPoint);

            const EHandle Candidates[3] = { EHandle::Point, EHandle::ArriveTangent, EHandle::LeaveTangent };
            const int32   NumCandidates = bTangentsPickable ? 3 : 1;

            for (int32 c = 0; c < NumCandidates; ++c)
            {
                ImVec2 Screen;
                if (!ProjectToScreen(ViewProjection, GetHandleWorldPosition(Spline, LocalToWorld, i, Candidates[c]), ViewportSize, Screen))
                {
                    continue;
                }

                const float DistSq = ScreenDistSq(Screen, MouseInViewport);
                if (DistSq < BestDistSq)
                {
                    BestDistSq    = DistSq;
                    OutPointIndex = i;
                    OutHandle     = Candidates[c];
                    bFound        = true;
                }
            }
        }

        return bFound;
    }

    void FSplineEditMode::Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize)
    {
        if (World == nullptr)
        {
            return;
        }

        const entt::entity Entity = FindSplineEntity(World);
        if (Entity != ActiveEntity)
        {
            ActiveEntity   = Entity;
            SelectedPoint  = INDEX_NONE;
            bGizmoUsedOnce = false;
        }

        if (ActiveEntity == entt::null)
        {
            return;
        }

        SSplineComponent* Spline = World->TryGetComponent<SSplineComponent>(ActiveEntity);
        STransformComponent* Transform = World->TryGetComponent<STransformComponent>(ActiveEntity);
        if (Spline == nullptr || Transform == nullptr)
        {
            return;
        }

        // A point removed from the details panel (or by undo) can leave the selection past the end.
        if (SelectedPoint >= static_cast<int32>(Spline->Points.size()))
        {
            SelectedPoint = INDEX_NONE;
        }

        const FMatrix4 LocalToWorld = Transform->GetWorldMatrix();
        const FMatrix4 WorldToLocal = Math::Inverse(LocalToWorld);

        FMatrix4 ViewMatrix       = Camera.GetViewMatrix();
        FMatrix4 ProjectionMatrix = Camera.GetProjectionMatrix();
        // Camera projection bakes Vulkan +Y-down NDC; ImGuizmo (and ProjectToScreen) expect GL convention.
        ProjectionMatrix[1][1] *= -1.0f;
        const FMatrix4 ViewProjection = ProjectionMatrix * ViewMatrix;

        const ImVec2 MousePos = ImGui::GetMousePos();
        const ImVec2 MouseInViewport(MousePos.x - ViewportScreenOrigin.x, MousePos.y - ViewportScreenOrigin.y);

        // Selection picking. Skipped while the gizmo is hovered or in use, so grabbing an axis never
        // re-picks whatever handle happens to sit under the cursor.
        const bool bGizmoBusy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
        if (bViewportHovered && !bGizmoBusy && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            int32   PickedPoint  = INDEX_NONE;
            EHandle PickedHandle = EHandle::Point;

            if (PickHandle(*Spline, LocalToWorld, ViewProjection, ViewportSize, MouseInViewport, PickedPoint, PickedHandle))
            {
                SelectedPoint  = PickedPoint;
                SelectedHandle = PickedHandle;
            }
            else if (ImGui::GetIO().KeyCtrl)
            {
                // Ctrl+click on the curve inserts a point there. Found by walking key space and projecting:
                // cheap, and it lands the new point exactly on the existing curve so the shape is preserved
                // at the moment of insertion.
                const int32 NumSegments = Spline->GetNumSegments();
                if (NumSegments > 0)
                {
                    constexpr int32 kStepsPerSegment = 24;
                    const int32 TotalSteps = NumSegments * kStepsPerSegment;

                    float BestDistSq = kPickRadiusPx * kPickRadiusPx * 4.0f;
                    float BestKey    = -1.0f;

                    for (int32 Step = 0; Step <= TotalSteps; ++Step)
                    {
                        const float Key = static_cast<float>(Step) / static_cast<float>(kStepsPerSegment);
                        const FVector3 WorldPos = FVector3(LocalToWorld * FVector4(Spline->EvaluatePosition(Key), 1.0f));

                        ImVec2 Screen;
                        if (!ProjectToScreen(ViewProjection, WorldPos, ViewportSize, Screen))
                        {
                            continue;
                        }

                        const float DistSq = ScreenDistSq(Screen, MouseInViewport);
                        if (DistSq < BestDistSq)
                        {
                            BestDistSq = DistSq;
                            BestKey    = Key;
                        }
                    }

                    if (BestKey >= 0.0f)
                    {
                        if (Context) { Context->BeginModeTransaction(); }

                        const int32 Segment = Math::Clamp(static_cast<int32>(Math::Floor(BestKey)), 0, NumSegments - 1);

                        SSplinePoint NewPoint;
                        NewPoint.Location    = Spline->EvaluatePosition(BestKey);
                        NewPoint.Scale       = Spline->EvaluateScale(BestKey);
                        NewPoint.Roll        = Spline->EvaluateRoll(BestKey);
                        NewPoint.TangentMode = ESplineTangentMode::Auto;

                        // Insert after the segment's first point, so the new point splits that segment.
                        const int32 InsertAt = Segment + 1;
                        Spline->Points.insert(Spline->Points.begin() + InsertAt, NewPoint);
                        Spline->UpdateTangents();

                        SelectedPoint  = InsertAt;
                        SelectedHandle = EHandle::Point;

                        if (Context) { Context->EndModeTransaction("Insert Spline Point"); }
                    }
                }
            }
            else
            {
                SelectedPoint = INDEX_NONE;
            }
        }

        // Delete the selected point.
        if (bViewportHovered && SelectedPoint != INDEX_NONE && !bGizmoBusy
            && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)))
        {
            if (Context) { Context->BeginModeTransaction(); }

            Spline->Points.erase(Spline->Points.begin() + SelectedPoint);
            Spline->UpdateTangents();
            SelectedPoint = INDEX_NONE;

            if (Context) { Context->EndModeTransaction("Delete Spline Point"); }
            return;
        }

        if (SelectedPoint == INDEX_NONE || Spline->Points.empty())
        {
            // Selection can vanish mid-drag (undo, a details-panel edit); close the transaction so the
            // next interaction is not folded into it.
            if (bGizmoUsedOnce)
            {
                if (Context) { Context->EndModeTransaction("Edit Spline"); }
                bGizmoUsedOnce = false;
            }
            return;
        }

        // Handles only ever translate: rotating or scaling a control point has no meaning.
        const FVector3 HandleWorld = GetHandleWorldPosition(*Spline, LocalToWorld, SelectedPoint, SelectedHandle);

        FMatrix4 HandleMatrix = Math::Translate(FMatrix4(1.0f), HandleWorld);

        ImGuizmo::Manipulate(Math::ValuePtr(ViewMatrix), Math::ValuePtr(ProjectionMatrix),
            ImGuizmo::TRANSLATE, ImGuizmo::WORLD, Math::ValuePtr(HandleMatrix));

        if (ImGuizmo::IsUsing())
        {
            if (!bGizmoUsedOnce)
            {
                if (Context) { Context->BeginModeTransaction(); }
                bGizmoUsedOnce = true;
            }

            const FVector3 NewWorld(HandleMatrix[3][0], HandleMatrix[3][1], HandleMatrix[3][2]);
            ApplyHandleWorldPosition(*Spline, WorldToLocal, SelectedPoint, SelectedHandle, NewWorld);

            // Auto/Linear neighbors re-derive from the moved point, so the curve updates live rather than
            // snapping into shape on release.
            Spline->UpdateTangents();
        }
        else if (bGizmoUsedOnce)
        {
            if (Context) { Context->EndModeTransaction("Edit Spline"); }
            bGizmoUsedOnce = false;
        }
    }

    void FSplineEditMode::DrawOverlay(CWorld* World, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera)
    {
        if (World == nullptr || ActiveEntity == entt::null)
        {
            return;
        }

        const SSplineComponent* Spline = World->TryGetComponent<SSplineComponent>(ActiveEntity);
        const STransformComponent* Transform = World->TryGetComponent<STransformComponent>(ActiveEntity);
        if (Spline == nullptr || Transform == nullptr || Spline->Points.empty())
        {
            return;
        }

        const FMatrix4 LocalToWorld = Transform->GetWorldMatrix();

        FMatrix4 ProjectionMatrix = Camera.GetProjectionMatrix();
        ProjectionMatrix[1][1] *= -1.0f;
        const FMatrix4 ViewProjection = ProjectionMatrix * Camera.GetViewMatrix();

        // 3D decoration for the selected point: its two tangent handles, whatever the tangent mode. The
        // component visualizer only draws handles for User points, which is right for an unselected
        // spline but hides the very thing you came here to grab.
        if (SelectedPoint != INDEX_NONE && SelectedPoint < static_cast<int32>(Spline->Points.size()))
        {
            const FVector3 PointWorld  = GetHandleWorldPosition(*Spline, LocalToWorld, SelectedPoint, EHandle::Point);
            const FVector3 ArriveWorld = GetHandleWorldPosition(*Spline, LocalToWorld, SelectedPoint, EHandle::ArriveTangent);
            const FVector3 LeaveWorld  = GetHandleWorldPosition(*Spline, LocalToWorld, SelectedPoint, EHandle::LeaveTangent);

            // Duration -1 = this frame only; the overlay re-emits every tick.
            World->DrawLine(PointWorld, ArriveWorld, kArriveColor, 2.0f, false, -1.0f);
            World->DrawLine(PointWorld, LeaveWorld,  kLeaveColor,  2.0f, false, -1.0f);
        }

        ImDrawList* DrawList = ImGui::GetWindowDrawList();

        for (int32 i = 0; i < static_cast<int32>(Spline->Points.size()); ++i)
        {
            const bool bSelectedPoint = (i == SelectedPoint);

            const EHandle Handles[3] = { EHandle::Point, EHandle::ArriveTangent, EHandle::LeaveTangent };
            const int32   NumHandles = bSelectedPoint ? 3 : 1;

            for (int32 h = 0; h < NumHandles; ++h)
            {
                ImVec2 Screen;
                if (!ProjectToScreen(ViewProjection, GetHandleWorldPosition(*Spline, LocalToWorld, i, Handles[h]), ViewportSize, Screen))
                {
                    continue;
                }

                const ImVec2 Pixel(ViewportScreenOrigin.x + Screen.x, ViewportScreenOrigin.y + Screen.y);
                const bool bIsActiveHandle = bSelectedPoint && (Handles[h] == SelectedHandle);

                ImU32 Color;
                float Radius;
                switch (Handles[h])
                {
                case EHandle::ArriveTangent: Color = IM_COL32( 90, 190, 255, 235); Radius = 4.5f; break;
                case EHandle::LeaveTangent:  Color = IM_COL32( 90, 255, 140, 235); Radius = 4.5f; break;
                default:
                    Color  = bSelectedPoint ? IM_COL32(255, 240, 90, 255) : IM_COL32(255, 200, 80, 210);
                    Radius = bSelectedPoint ? 7.0f : 5.0f;
                    break;
                }

                DrawList->AddCircleFilled(Pixel, Radius, Color, 12);
                if (bIsActiveHandle)
                {
                    DrawList->AddCircle(Pixel, Radius + 3.0f, IM_COL32(255, 255, 255, 230), 16, 2.0f);
                }

                if (bShowPointIndices && Handles[h] == EHandle::Point)
                {
                    char Label[16];
                    ImFormatString(Label, sizeof(Label), "%d", i);
                    DrawList->AddText(ImVec2(Pixel.x + Radius + 3.0f, Pixel.y - 8.0f), IM_COL32(20, 20, 20, 220), Label);
                    DrawList->AddText(ImVec2(Pixel.x + Radius + 2.0f, Pixel.y - 9.0f), IM_COL32(255, 255, 255, 235), Label);
                }
            }
        }
    }

    void FSplineEditMode::DrawToolbar(CWorld* World, float ButtonSize)
    {
        if (World == nullptr)
        {
            return;
        }

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        SSplineComponent* Spline = (ActiveEntity != entt::null) ? World->TryGetComponent<SSplineComponent>(ActiveEntity) : nullptr;
        if (Spline == nullptr)
        {
            ImGui::TextDisabled("Select an entity with a Spline component");
            return;
        }

        const bool bHasSelection = (SelectedPoint != INDEX_NONE) && (SelectedPoint < static_cast<int32>(Spline->Points.size()));

        // Append a point past the end, continuing the curve's direction so it lands somewhere visible
        // rather than on top of the last point.
        if (ImGui::Button(LE_ICON_VECTOR_POINT_PLUS, ImVec2(ButtonSize, ButtonSize)))
        {
            if (Context) { Context->BeginModeTransaction(); }

            SSplinePoint NewPoint;
            if (Spline->Points.empty())
            {
                NewPoint.Location = FVector3(0.0f);
            }
            else
            {
                const SSplinePoint& Last = Spline->Points.back();
                FVector3 Direction = Last.LeaveTangent;
                if (Math::LengthSquared(Direction) <= 1.0e-6f)
                {
                    Direction = (Spline->Points.size() >= 2)
                                    ? (Last.Location - Spline->Points[Spline->Points.size() - 2].Location)
                                    : FVector3(0.0f, 0.0f, 1.0f);
                }
                if (Math::LengthSquared(Direction) <= 1.0e-6f)
                {
                    Direction = FVector3(0.0f, 0.0f, 1.0f);
                }

                NewPoint.Location = Last.Location + Math::Normalize(Direction) * 1.0f;
                NewPoint.Scale    = Last.Scale;
                NewPoint.Roll     = Last.Roll;
            }

            Spline->Points.push_back(NewPoint);
            Spline->UpdateTangents();
            SelectedPoint  = static_cast<int32>(Spline->Points.size()) - 1;
            SelectedHandle = EHandle::Point;

            if (Context) { Context->EndModeTransaction("Add Spline Point"); }
        }
        HoverTooltip("Add a point at the end of the spline");

        ImGui::SameLine();
        ImGui::BeginDisabled(!bHasSelection);
        if (ImGui::Button(LE_ICON_VECTOR_POINT_MINUS, ImVec2(ButtonSize, ButtonSize)))
        {
            if (Context) { Context->BeginModeTransaction(); }

            Spline->Points.erase(Spline->Points.begin() + SelectedPoint);
            Spline->UpdateTangents();
            SelectedPoint = INDEX_NONE;

            if (Context) { Context->EndModeTransaction("Delete Spline Point"); }
        }
        ImGui::EndDisabled();
        HoverTooltip("Delete the selected point (Del)");

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Tangent mode for the selected point. Resetting to Auto/Linear is the way back out of a hand-
        // authored tangent, so it needs to be one click away.
        ImGui::BeginDisabled(!bHasSelection);
        {
            static const char* kModeNames[] = { "Auto", "Linear", "User" };
            const int32 CurrentMode = bHasSelection ? static_cast<int32>(Spline->Points[SelectedPoint].TangentMode) : 0;

            ImGui::SetNextItemWidth(90.0f);
            int32 NewMode = CurrentMode;
            if (ImGui::Combo("##SplineTangentMode", &NewMode, kModeNames, IM_ARRAYSIZE(kModeNames)) && bHasSelection)
            {
                if (Context) { Context->BeginModeTransaction(); }

                Spline->Points[SelectedPoint].TangentMode = static_cast<ESplineTangentMode>(NewMode);
                Spline->UpdateTangents();

                if (Context) { Context->EndModeTransaction("Set Tangent Mode"); }
            }
        }
        ImGui::EndDisabled();
        HoverTooltip("Tangent mode for the selected point");

        ImGui::SameLine();
        bool bClosedLoop = Spline->bClosedLoop;
        if (ImGui::Checkbox("Closed", &bClosedLoop))
        {
            if (Context) { Context->BeginModeTransaction(); }

            Spline->bClosedLoop = bClosedLoop;
            Spline->UpdateTangents();

            if (Context) { Context->EndModeTransaction("Toggle Spline Loop"); }
        }
        HoverTooltip("Join the last point back to the first");

        ImGui::SameLine();
        if (ImGui::Button("Reset Tangents"))
        {
            if (Context) { Context->BeginModeTransaction(); }

            for (SSplinePoint& Point : Spline->Points)
            {
                if (Point.TangentMode == ESplineTangentMode::User)
                {
                    Point.TangentMode = ESplineTangentMode::Auto;
                }
            }
            Spline->UpdateTangents();

            if (Context) { Context->EndModeTransaction("Reset Spline Tangents"); }
        }
        HoverTooltip("Return every hand-authored tangent to Auto");

        ImGui::SameLine();
        ImGui::Checkbox("Indices", &bShowPointIndices);

        ImGui::SameLine();
        ImGui::TextDisabled("|  %d points  |  Ctrl+Click curve to insert", static_cast<int32>(Spline->Points.size()));
    }
}
