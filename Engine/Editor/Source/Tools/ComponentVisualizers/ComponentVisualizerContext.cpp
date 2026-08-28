#include "EditorPCH.h"
#include "ComponentVisualizerContext.h"

#include "Core/Object/Class.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Renderer/ViewVolume.h"
#include "World/Entity/Components/CameraComponent.h"

#include <imgui_internal.h>

namespace Lumina
{
    namespace
    {
        constexpr float kEpsilon = 1e-6f;

        ImU32 ToU32(const FVector4& Color)
        {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(Color.x, Color.y, Color.z, Color.w));
        }

        FVector4 WithAlpha(const FVector4& Color, float Alpha)
        {
            return FVector4(Color.x, Color.y, Color.z, Alpha);
        }

        bool IntersectPlane(const FVector3& RayOrigin, const FVector3& RayDirection,
            const FVector3& PlanePoint, const FVector3& PlaneNormal, float& OutDistance)
        {
            const float Denominator = Math::Dot(RayDirection, PlaneNormal);
            if (Math::Abs(Denominator) < kEpsilon)
            {
                return false;
            }

            OutDistance = Math::Dot(PlanePoint - RayOrigin, PlaneNormal) / Denominator;
            return OutDistance > 0.0f;
        }
    }

    void FVisualizerView::SetFromCamera(const SCameraComponent& Camera, ImVec2 InOrigin, ImVec2 InSize)
    {
        Origin = InOrigin;
        Size = ImVec2(Math::Max(InSize.x, 1.0f), Math::Max(InSize.y, 1.0f));

        ViewMatrix = Camera.GetViewMatrix();
        ProjectionMatrix = Camera.GetProjectionMatrix();
        ProjectionMatrix[1][1] *= -1.0f;
        ViewProjection = ProjectionMatrix * ViewMatrix;

        const FViewVolume& Volume = Camera.GetViewVolume();
        CameraLocation = Camera.GetPosition();
        CameraForward = Volume.GetForwardVector();
        CameraUp = Volume.GetUpVector();
        CameraRight = Math::Normalize(Math::Cross(CameraUp, CameraForward));

        TanHalfFov = std::tan(Math::Radians(Volume.GetFOV()) * 0.5f);
        bOrthographic = Volume.IsOrthographic();
        OrthoWidth = Volume.GetOrthoWidth();
    }

    bool FVisualizerView::WorldToScreen(const FVector3& World, ImVec2& OutScreen) const
    {
        const FVector4 Clip = ViewProjection * FVector4(World, 1.0f);
        if (Clip.w <= kEpsilon)
        {
            return false;
        }

        const FVector3 Ndc = FVector3(Clip) / Clip.w;
        OutScreen = ImVec2(Origin.x + (Ndc.x * 0.5f + 0.5f) * Size.x,
                           Origin.y + (0.5f - Ndc.y * 0.5f) * Size.y);
        return true;
    }

    void FVisualizerView::ScreenToRay(ImVec2 Screen, FVector3& OutOrigin, FVector3& OutDirection) const
    {
        const float Sx = ((Screen.x - Origin.x) / Size.x) * 2.0f - 1.0f;
        const float Sy = 1.0f - ((Screen.y - Origin.y) / Size.y) * 2.0f;
        const float AspectRatio = Size.x / Size.y;

        if (bOrthographic)
        {
            const float HalfWidth = OrthoWidth * 0.5f;
            const float HalfHeight = HalfWidth / Math::Max(AspectRatio, 0.001f);

            OutOrigin = CameraLocation + CameraRight * (Sx * HalfWidth) + CameraUp * (Sy * HalfHeight);
            OutDirection = CameraForward;
            return;
        }

        OutOrigin = CameraLocation;
        OutDirection = Math::Normalize(CameraForward
                                     + CameraRight * (Sx * TanHalfFov * AspectRatio)
                                     + CameraUp * (Sy * TanHalfFov));
    }

    float FVisualizerView::WorldPerPixelAt(const FVector3& World) const
    {
        if (bOrthographic)
        {
            return OrthoWidth / Math::Max(Size.x, 1.0f);
        }

        const float Depth = Math::Max(Math::Dot(World - CameraLocation, CameraForward), 0.01f);
        return (2.0f * Depth * TanHalfFov) / Math::Max(Size.y, 1.0f);
    }

    bool FVisualizerView::IsFacingCamera(const FVector3& Point, const FVector3& Normal) const
    {
        return Math::Dot(DirectionToCamera(Point), Normal) > 0.0f;
    }

    FVector3 FVisualizerView::DirectionToCamera(const FVector3& Point) const
    {
        return bOrthographic ? -CameraForward : Math::Normalize(CameraLocation - Point);
    }

    void FVisualizerInteractionState::ResetDrag()
    {
        ActiveKey = 0;
        ActiveEntity = ECS::NullEntity;
        ActiveComponentType = nullptr;
        GrabAnchor = FVector3(0.0f);
        GrabAxis = FVector3(0.0f);
        GrabPlaneNormal = FVector3(0.0f);
        GrabOffset = FVector3(0.0f);
        LastPosition = FVector3(0.0f);
        bTransactionOpen = false;
        EditLabel = FName();
    }

    void FVisualizerInteractionState::ClearSelection()
    {
        SelectedEntity = ECS::NullEntity;
        SelectedComponentType = nullptr;
        SelectedSubElement = INDEX_NONE;
    }

    void FVisualizerInteractionState::Reset()
    {
        ResetDrag();
        ClearSelection();
        HoveredKey = 0;
        PendingHoveredKey = 0;
        PendingHoveredPriority = -1;
        PendingHoveredScore = 0.0f;
        bCapturedInput = false;
    }

    void FVisualizerInteractionState::BeginPass()
    {
        bCapturedInput = false;
        bActiveHandleSeen = false;
    }

    void FVisualizerInteractionState::EndPass()
    {
        HoveredKey = PendingHoveredKey;
        PendingHoveredKey = 0;
        PendingHoveredPriority = -1;
        PendingHoveredScore = 0.0f;
    }

    FComponentVisualizerContext::FComponentVisualizerContext(FVisualizerInteractionState& InState, IComponentVisualizerHost* InHost)
        : State(InState)
        , Host(InHost)
    {
    }

    void FComponentVisualizerContext::SetTarget(ECS::FEntity InEntity, CStruct* InComponentType)
    {
        Entity = InEntity;
        ComponentType = InComponentType;
    }

    uint64 FComponentVisualizerContext::MakeKey(uint32 ID) const
    {
        uint64 Key = (uint64)Entity.GetPacked();
        Key = (Key * 0x9E3779B97F4A7C15ull) ^ (uint64)(uintptr_t)ComponentType;
        Key = (Key * 0x9E3779B97F4A7C15ull) ^ (uint64)ID;

        // Zero is the no-handle sentinel, so no key may land on it.
        return Key | 1ull;
    }

    void FComponentVisualizerContext::Line(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness)
    {
        ImVec2 A, B;
        if (DrawList == nullptr || !View.WorldToScreen(Start, A) || !View.WorldToScreen(End, B))
        {
            return;
        }

        DrawList->AddLine(A, B, ToU32(Color), Thickness);
    }

    void FComponentVisualizerContext::DashedLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness, float DashPixels)
    {
        ImVec2 A, B;
        if (DrawList == nullptr || !View.WorldToScreen(Start, A) || !View.WorldToScreen(End, B))
        {
            return;
        }

        const float Length = Math::Sqrt((B.x - A.x) * (B.x - A.x) + (B.y - A.y) * (B.y - A.y));
        const int32 Steps = Math::Clamp((int32)(Length / Math::Max(DashPixels, 1.0f)), 1, 512);
        const ImU32 Packed = ToU32(Color);

        for (int32 i = 0; i < Steps; i += 2)
        {
            const float T0 = (float)i / (float)Steps;
            const float T1 = Math::Min((float)(i + 1) / (float)Steps, 1.0f);
            const ImVec2 P0(A.x + (B.x - A.x) * T0, A.y + (B.y - A.y) * T0);
            const ImVec2 P1(A.x + (B.x - A.x) * T1, A.y + (B.y - A.y) * T1);
            DrawList->AddLine(P0, P1, Packed, Thickness);
        }
    }

    void FComponentVisualizerContext::Circle(const FVector3& Center, float PixelRadius, const FVector4& Color, bool bFilled, float Thickness)
    {
        ImVec2 Screen;
        if (DrawList == nullptr || !View.WorldToScreen(Center, Screen))
        {
            return;
        }

        if (bFilled)
        {
            DrawList->AddCircleFilled(Screen, PixelRadius, ToU32(Color), 0);
        }
        else
        {
            DrawList->AddCircle(Screen, PixelRadius, ToU32(Color), 0, Thickness);
        }
    }

    void FComponentVisualizerContext::Polygon(const FVector3* Corners, int32 Count, const FVector4& Fill, const FVector4& Outline, float OutlineThickness)
    {
        if (DrawList == nullptr || Corners == nullptr || Count < 3 || Count > 32)
        {
            return;
        }

        ImVec2 Screen[32];
        for (int32 i = 0; i < Count; ++i)
        {
            if (!View.WorldToScreen(Corners[i], Screen[i]))
            {
                return;
            }
        }

        if (Fill.w > 0.0f)
        {
            DrawList->AddConvexPolyFilled(Screen, Count, ToU32(Fill));
        }

        if (Outline.w > 0.0f)
        {
            DrawList->AddPolyline(Screen, Count, ToU32(Outline), ImDrawFlags_Closed, OutlineThickness);
        }
    }

    void FComponentVisualizerContext::Quad(const FVector3& A, const FVector3& B, const FVector3& C, const FVector3& D,
        const FVector4& Fill, const FVector4& Outline, float OutlineThickness)
    {
        const FVector3 Corners[4] = { A, B, C, D };
        Polygon(Corners, 4, Fill, Outline, OutlineThickness);
    }

    void FComponentVisualizerContext::Text(const FVector3& World, const FVector4& Color, const char* Format, ...)
    {
        ImVec2 Screen;
        if (DrawList == nullptr || !View.WorldToScreen(World, Screen))
        {
            return;
        }

        char Buffer[256];
        va_list Args;
        va_start(Args, Format);
        ImFormatStringV(Buffer, sizeof(Buffer), Format, Args);
        va_end(Args);

        DrawList->AddText(ImVec2(Screen.x + 1.0f, Screen.y + 1.0f), IM_COL32(0, 0, 0, 190), Buffer);
        DrawList->AddText(Screen, ToU32(Color), Buffer);
    }

    void FComponentVisualizerContext::Label(const FVector3& World, const FVector4& Color, const char* Format, ...)
    {
        ImVec2 Screen;
        if (DrawList == nullptr || !View.WorldToScreen(World, Screen))
        {
            return;
        }

        char Buffer[256];
        va_list Args;
        va_start(Args, Format);
        ImFormatStringV(Buffer, sizeof(Buffer), Format, Args);
        va_end(Args);

        const ImVec2 TextSize = ImGui::CalcTextSize(Buffer);
        const ImVec2 Padding(5.0f, 3.0f);
        const ImVec2 Min(Screen.x - Padding.x, Screen.y - Padding.y);
        const ImVec2 Max(Screen.x + TextSize.x + Padding.x, Screen.y + TextSize.y + Padding.y);

        DrawList->AddRectFilled(Min, Max, IM_COL32(12, 14, 18, 205), 3.0f);
        DrawList->AddRect(Min, Max, ToU32(WithAlpha(Color, 0.45f)), 3.0f);
        DrawList->AddText(Screen, ToU32(Color), Buffer);
    }

    void FComponentVisualizerContext::Measurement(const FVector3& Start, const FVector3& End, const FVector4& Color, const char* Format, ...)
    {
        ImVec2 A, B;
        if (DrawList == nullptr || !View.WorldToScreen(Start, A) || !View.WorldToScreen(End, B))
        {
            return;
        }

        const ImU32 Packed = ToU32(Color);
        DrawList->AddLine(A, B, Packed, 1.5f);

        const float DX = B.x - A.x;
        const float DY = B.y - A.y;
        const float Length = Math::Max(Math::Sqrt(DX * DX + DY * DY), 0.001f);
        const ImVec2 Tick(-DY / Length * 5.0f, DX / Length * 5.0f);

        DrawList->AddLine(ImVec2(A.x - Tick.x, A.y - Tick.y), ImVec2(A.x + Tick.x, A.y + Tick.y), Packed, 1.5f);
        DrawList->AddLine(ImVec2(B.x - Tick.x, B.y - Tick.y), ImVec2(B.x + Tick.x, B.y + Tick.y), Packed, 1.5f);

        char Buffer[128];
        va_list Args;
        va_start(Args, Format);
        ImFormatStringV(Buffer, sizeof(Buffer), Format, Args);
        va_end(Args);

        const ImVec2 TextSize = ImGui::CalcTextSize(Buffer);
        const ImVec2 Center((A.x + B.x) * 0.5f - TextSize.x * 0.5f, (A.y + B.y) * 0.5f - TextSize.y * 0.5f);
        const ImVec2 Padding(4.0f, 2.0f);

        DrawList->AddRectFilled(ImVec2(Center.x - Padding.x, Center.y - Padding.y),
            ImVec2(Center.x + TextSize.x + Padding.x, Center.y + TextSize.y + Padding.y),
            IM_COL32(12, 14, 18, 215), 3.0f);
        DrawList->AddText(Center, Packed, Buffer);
    }

    bool FComponentVisualizerContext::IsSubElementSelected(int32 SubElement) const
    {
        return State.SelectedEntity == Entity
            && State.SelectedComponentType == ComponentType
            && State.SelectedSubElement == SubElement;
    }

    int32 FComponentVisualizerContext::GetSelectedSubElement() const
    {
        if (State.SelectedEntity != Entity || State.SelectedComponentType != ComponentType)
        {
            return INDEX_NONE;
        }

        return State.SelectedSubElement;
    }

    void FComponentVisualizerContext::SelectSubElement(int32 SubElement)
    {
        State.SelectedEntity = Entity;
        State.SelectedComponentType = ComponentType;
        State.SelectedSubElement = SubElement;
    }

    void FComponentVisualizerContext::ClearSubElementSelection()
    {
        State.SelectedEntity = ECS::NullEntity;
        State.SelectedComponentType = nullptr;
        State.SelectedSubElement = INDEX_NONE;
    }

    void FComponentVisualizerContext::NameEdit(FName Label)
    {
        State.EditLabel = Label;
    }

    void FComponentVisualizerContext::BeginEdit(FName Label)
    {
        if (Host == nullptr || State.bTransactionOpen)
        {
            return;
        }

        Host->BeginVisualizerTransaction(Entity, ComponentType);
        State.bTransactionOpen = true;
        State.EditLabel = Label;
    }

    void FComponentVisualizerContext::EndEdit()
    {
        if (Host == nullptr || !State.bTransactionOpen || State.IsDragging())
        {
            return;
        }

        Host->EndVisualizerTransaction(State.EditLabel.IsNone() ? FName("Edit Component") : State.EditLabel);
        State.bTransactionOpen = false;
        State.EditLabel = FName();
    }

    void FComponentVisualizerContext::MarkDirty()
    {
        if (Host != nullptr)
        {
            Host->MarkVisualizerSceneDirty();
        }
    }

    bool FComponentVisualizerContext::SolveConstraint(const FHandleDesc& Desc, FVector3& OutPosition) const
    {
        FVector3 RayOrigin, RayDirection;
        View.ScreenToRay(ImGui::GetMousePos(), RayOrigin, RayDirection);

        switch (Desc.Constraint)
        {
        case EHandleConstraint::Axis:
            {
                // The captured axis, not the live one, so orbiting mid-drag cannot re-aim the constraint.
                const FVector3 Axis = State.GrabAxis;
                const FVector3 ToAnchor = State.GrabAnchor - RayOrigin;

                const float AxisDotRay = Math::Dot(Axis, RayDirection);
                const float Denominator = 1.0f - AxisDotRay * AxisDotRay;
                if (Math::Abs(Denominator) < kEpsilon)
                {
                    return false;
                }

                const float AlongAxis = (AxisDotRay * Math::Dot(RayDirection, ToAnchor) - Math::Dot(Axis, ToAnchor)) / Denominator;
                OutPosition = State.GrabAnchor + Axis * AlongAxis;
                return true;
            }

        case EHandleConstraint::Plane:
            {
                float Distance = 0.0f;
                if (!IntersectPlane(RayOrigin, RayDirection, State.GrabAnchor, State.GrabPlaneNormal, Distance))
                {
                    return false;
                }

                OutPosition = RayOrigin + RayDirection * Distance;
                return true;
            }

        default:
            {
                float Distance = 0.0f;
                if (!IntersectPlane(RayOrigin, RayDirection, State.GrabAnchor, -View.CameraForward, Distance))
                {
                    return false;
                }

                OutPosition = RayOrigin + RayDirection * Distance;
                return true;
            }
        }
    }

    void FComponentVisualizerContext::OfferHover(uint64 Key, int32 Priority, float Score) const
    {
        if (Priority < State.PendingHoveredPriority)
        {
            return;
        }

        if (Priority == State.PendingHoveredPriority && State.PendingHoveredKey != 0 && Score >= State.PendingHoveredScore)
        {
            return;
        }

        State.PendingHoveredKey = Key;
        State.PendingHoveredPriority = Priority;
        State.PendingHoveredScore = Score;
    }

    void FComponentVisualizerContext::BeginDrag(const FHandleDesc& Desc, uint64 Key)
    {
        State.ActiveKey = Key;
        State.ActiveEntity = Entity;
        State.ActiveComponentType = ComponentType;
        State.GrabAnchor = Desc.Position;
        State.GrabAxis = (Desc.Constraint == EHandleConstraint::Axis) ? Math::Normalize(Desc.Axis) : FVector3(0.0f);
        State.GrabPlaneNormal = (Desc.Constraint == EHandleConstraint::Plane) ? Math::Normalize(Desc.Normal) : -View.CameraForward;
        State.LastPosition = Desc.Position;
        State.GrabOffset = FVector3(0.0f);
        State.LastPosition = Desc.Position;
        State.EditLabel = FName();

        FVector3 Raw;
        if (SolveConstraint(Desc, Raw))
        {
            State.GrabOffset = Desc.Position - Raw;
        }

    }

    void FComponentVisualizerContext::EndDrag()
    {
        if (Host != nullptr && State.bTransactionOpen)
        {
            Host->EndVisualizerTransaction(State.EditLabel.IsNone() ? FName("Edit Component") : State.EditLabel);
        }

        State.ResetDrag();
    }

    void FComponentVisualizerContext::DrawHandleGlyph(const FHandleDesc& Desc, const ImVec2& Screen, const FVector4& Color) const
    {
        if (DrawList == nullptr || !Desc.Style->bDrawGlyph)
        {
            return;
        }

        const float Radius = Desc.Style->PixelRadius;
        const ImU32 Fill = ToU32(Color);
        constexpr ImU32 Outline = IM_COL32(10, 12, 16, 225);

        switch (Desc.Style->Shape)
        {
        case EVisualizerHandleShape::Square:
            {
                const ImVec2 Min(Screen.x - Radius, Screen.y - Radius);
                const ImVec2 Max(Screen.x + Radius, Screen.y + Radius);
                DrawList->AddRectFilled(Min, Max, Fill, 1.5f);
                DrawList->AddRect(Min, Max, Outline, 1.5f, 0, 1.5f);
            }
            break;

        case EVisualizerHandleShape::Diamond:
            {
                const ImVec2 Points[4] =
                {
                    ImVec2(Screen.x, Screen.y - Radius),
                    ImVec2(Screen.x + Radius, Screen.y),
                    ImVec2(Screen.x, Screen.y + Radius),
                    ImVec2(Screen.x - Radius, Screen.y),
                };
                DrawList->AddConvexPolyFilled(Points, 4, Fill);
                DrawList->AddPolyline(Points, 4, Outline, ImDrawFlags_Closed, 1.5f);
            }
            break;

        default:
            DrawList->AddCircleFilled(Screen, Radius, Fill, 0);
            DrawList->AddCircle(Screen, Radius, Outline, 0, 1.5f);
            break;
        }
    }

    void FComponentVisualizerContext::DrawFaceSurface(const FHandleDesc& Desc, const FVector4& Color, bool bHot, bool bSelected)
    {
        const FVector3 Corners[4] =
        {
            Desc.Position - Desc.HalfU - Desc.HalfV,
            Desc.Position + Desc.HalfU - Desc.HalfV,
            Desc.Position + Desc.HalfU + Desc.HalfV,
            Desc.Position - Desc.HalfU + Desc.HalfV,
        };

        float Opacity = bSelected ? Desc.Style->SurfaceOpacity * 2.2f : Desc.Style->SurfaceOpacity;
        if (bHot)
        {
            Opacity *= 2.4f;
        }

        const float OutlineThickness = (bHot || bSelected) ? 2.5f : 1.25f;
        const FVector4 OutlineColor = WithAlpha(Color, (bHot || bSelected) ? 1.0f : 0.55f);

        Polygon(Corners, 4, WithAlpha(Color, Math::Min(Opacity, 0.85f)), OutlineColor, OutlineThickness);
    }

    FVisualizerHandleResult FComponentVisualizerContext::ProcessHandle(const FHandleDesc& Desc)
    {
        FVisualizerHandleResult Result;
        Result.Position = Desc.Position;

        if (Registry == nullptr || DrawList == nullptr)
        {
            return Result;
        }

        const FVisualizerHandleStyle& Style = *Desc.Style;
        const uint64 Key = MakeKey(Desc.ID);
        const bool bWasActive = (State.ActiveKey == Key);

        // A face pointing away from the camera offers neither a surface nor a grab dot.
        const bool bCulled = Desc.bFaceSurface && !bWasActive
                          && !View.IsFacingCamera(Desc.Position, Math::Normalize(Desc.Normal));

        if (bWasActive)
        {
            Result.bActive = true;
            State.bCapturedInput = true;
            State.bActiveHandleSeen = true;

            FVector3 Raw;
            if (SolveConstraint(Desc, Raw))
            {
                const FVector3 Solved = Raw + State.GrabOffset;

                Result.Position = Solved;
                Result.Delta = Solved - State.LastPosition;
                Result.TotalDelta = Solved - State.GrabAnchor;
                Result.bChanged = Math::LengthSquared(Result.Delta) > 0.0f;

                if (Desc.Constraint == EHandleConstraint::Axis)
                {
                    Result.ScalarDelta = Math::Dot(Result.Delta, State.GrabAxis);
                    Result.TotalScalar = Math::Dot(Result.TotalDelta, State.GrabAxis);
                }

                State.LastPosition = Solved;

                // Opened on the first frame that actually moves, so a click that only selects records nothing.
                if (Result.bChanged && Host != nullptr && !State.bTransactionOpen)
                {
                    Host->BeginVisualizerTransaction(Entity, ComponentType);
                    State.bTransactionOpen = true;
                }
            }

            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                Result.bActive = false;
                Result.bReleased = true;
                EndDrag();
            }
        }
        else
        {
            Result.bHovered = (State.HoveredKey == Key);

            if (bInputEnabled && bViewportHovered && !State.IsDragging() && !bCulled)
            {
                ImVec2 Screen;
                if (View.WorldToScreen(Desc.Position, Screen))
                {
                    const ImVec2 Mouse = ImGui::GetMousePos();
                    const float DX = Mouse.x - Screen.x;
                    const float DY = Mouse.y - Screen.y;
                    const float DistanceSquared = DX * DX + DY * DY;
                    if (DistanceSquared <= Style.GrabPixelRadius * Style.GrabPixelRadius)
                    {
                        OfferHover(Key, 1, DistanceSquared);
                    }
                }
            }

            if (Result.bHovered)
            {
                State.bCapturedInput = true;

                if (Style.Tooltip != nullptr)
                {
                    ImGui::SetTooltip("%s", Style.Tooltip);
                }

                if (bInputEnabled && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    BeginDrag(Desc, Key);
                    Result.bPressed = true;
                    Result.bActive = true;
                }
            }
        }

        if (bCulled)
        {
            return Result;
        }

        const bool bHot = Result.bHovered || Result.bActive;
        const FVector4 Color = Result.bActive ? Style.ActiveColor : (Result.bHovered ? Style.HoverColor : Style.Color);

        if (Desc.bFaceSurface)
        {
            DrawFaceSurface(Desc, Color, bHot, IsSubElementSelected((int32)Desc.ID));
        }

        ImVec2 Screen;
        if (View.WorldToScreen(Result.Position, Screen))
        {
            DrawHandleGlyph(Desc, Screen, Color);
        }

        return Result;
    }

    FVisualizerHandleResult FComponentVisualizerContext::PointHandle(uint32 ID, const FVector3& World, const FVisualizerHandleStyle& Style)
    {
        FHandleDesc Desc;
        Desc.ID = ID;
        Desc.Position = World;
        Desc.Constraint = EHandleConstraint::Screen;
        Desc.Style = &Style;
        return ProcessHandle(Desc);
    }

    FVisualizerHandleResult FComponentVisualizerContext::AxisHandle(uint32 ID, const FVector3& World, const FVector3& Axis, const FVisualizerHandleStyle& Style)
    {
        FHandleDesc Desc;
        Desc.ID = ID;
        Desc.Position = World;
        Desc.Axis = Axis;
        Desc.Constraint = EHandleConstraint::Axis;
        Desc.Style = &Style;
        return ProcessHandle(Desc);
    }

    FVisualizerHandleResult FComponentVisualizerContext::PlaneHandle(uint32 ID, const FVector3& World, const FVector3& Normal, const FVisualizerHandleStyle& Style)
    {
        FHandleDesc Desc;
        Desc.ID = ID;
        Desc.Position = World;
        Desc.Normal = Normal;
        Desc.Constraint = EHandleConstraint::Plane;
        Desc.Style = &Style;
        return ProcessHandle(Desc);
    }

    FVisualizerHandleResult FComponentVisualizerContext::FaceHandle(uint32 ID, const FVector3& Center, const FVector3& Normal,
        const FVector3& HalfU, const FVector3& HalfV, const FVisualizerHandleStyle& Style)
    {
        FHandleDesc Desc;
        Desc.ID = ID;
        Desc.Position = Center;
        Desc.Normal = Normal;
        Desc.Axis = Normal;
        Desc.HalfU = HalfU;
        Desc.HalfV = HalfV;
        Desc.Constraint = EHandleConstraint::Axis;
        Desc.bFaceSurface = true;
        Desc.Style = &Style;

        FVisualizerHandleResult Result = ProcessHandle(Desc);
        if (Result.bPressed)
        {
            SelectSubElement((int32)ID);
        }

        return Result;
    }

    void FComponentVisualizerContext::FinishPass()
    {
        // A drag outlives its visualizer, so release it here rather than stranding the transaction.
        if (State.IsDragging() && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !State.bActiveHandleSeen))
        {
            EndDrag();
        }

        // Same for a panel widget whose transaction never saw its deactivate frame.
        if (State.bTransactionOpen && !State.IsDragging() && !ImGui::IsAnyItemActive())
        {
            EndEdit();
        }

        State.EndPass();
    }

    bool FComponentVisualizerContext::BeginPanel(const char* ID, const FVector3& WorldAnchor, ImVec2 PixelOffset)
    {
        ImVec2 Screen;
        if (!View.WorldToScreen(WorldAnchor, Screen))
        {
            return false;
        }

        ImGui::SetCursorScreenPos(ImVec2(Screen.x + PixelOffset.x, Screen.y + PixelOffset.y));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.06f, 0.08f, 0.92f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 6.0f));

        ImGui::BeginChild(ID, ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

        bPanelOpen = true;
        return true;
    }

    void FComponentVisualizerContext::EndPanel()
    {
        if (!bPanelOpen)
        {
            return;
        }

        const bool bPanelHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        bPanelOpen = false;

        // Widgets in the panel must not fall through to picking or the gizmo underneath it.
        if (bPanelHovered)
        {
            State.bCapturedInput = true;
        }
    }
}
