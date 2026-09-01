#pragma once

#include "Lumina.h"
#include "Core/Math/Math.h"
#include "World/ECS/Registry.h"

#define USE_IMGUI_API
#include <imgui.h>

namespace Lumina
{
    class CStruct;
    class CWorld;
    class IPrimitiveDrawInterface;
    struct SCameraComponent;

    // Camera and viewport rect a visualizer projects through, replacing the per-tool ProjectToScreen copies.
    struct EDITOR_API FVisualizerView
    {
        FMatrix4 ViewMatrix = FMatrix4(1.0f);
        FMatrix4 ProjectionMatrix = FMatrix4(1.0f);
        FMatrix4 ViewProjection = FMatrix4(1.0f);

        FVector3 CameraLocation = FVector3(0.0f);
        FVector3 CameraForward = FVector3(0.0f, 0.0f, 1.0f);
        FVector3 CameraRight = FVector3(1.0f, 0.0f, 0.0f);
        FVector3 CameraUp = FVector3(0.0f, 1.0f, 0.0f);

        ImVec2 Origin = ImVec2(0.0f, 0.0f);
        ImVec2 Size = ImVec2(1.0f, 1.0f);

        float TanHalfFov = 0.5f;
        float OrthoWidth = 0.0f;
        bool  bOrthographic = false;

        // ProjectionMatrix is stored with the Vulkan +Y-down flip already undone, matching ImGuizmo.
        void SetFromCamera(const SCameraComponent& Camera, ImVec2 InOrigin, ImVec2 InSize);

        NODISCARD bool WorldToScreen(const FVector3& World, ImVec2& OutScreen) const;
        void ScreenToRay(ImVec2 Screen, FVector3& OutOrigin, FVector3& OutDirection) const;

        // World units covered by one pixel at that depth, for constant-size handles and labels.
        NODISCARD float WorldPerPixelAt(const FVector3& World) const;

        NODISCARD bool IsFacingCamera(const FVector3& Point, const FVector3& Normal) const;
        NODISCARD FVector3 DirectionToCamera(const FVector3& Point) const;
    };

    enum class EVisualizerHandleShape : uint8
    {
        Circle,
        Square,
        Diamond,
    };

    struct FVisualizerHandleStyle
    {
        FVector4 Color = FVector4(0.30f, 0.68f, 1.00f, 0.95f);
        FVector4 HoverColor = FVector4(1.00f, 0.78f, 0.25f, 1.00f);
        FVector4 ActiveColor = FVector4(1.00f, 0.95f, 0.55f, 1.00f);

        float PixelRadius = 5.5f;
        float GrabPixelRadius = 11.0f;

        EVisualizerHandleShape Shape = EVisualizerHandleShape::Circle;

        // Face fill alpha is scaled from this; the outline always uses the full color.
        float SurfaceOpacity = 0.14f;

        const char* Tooltip = nullptr;

        bool bDrawGlyph = true;
    };

    struct FVisualizerHandleResult
    {
        bool bHovered = false;
        bool bActive = false;
        bool bPressed = false;
        bool bReleased = false;
        bool bChanged = false;

        FVector3 Position = FVector3(0.0f);
        FVector3 Delta = FVector3(0.0f);
        FVector3 TotalDelta = FVector3(0.0f);

        float ScalarDelta = 0.0f;
        float TotalScalar = 0.0f;

        NODISCARD explicit operator bool() const { return bChanged; }
    };

    // Editor services a visualizer reaches without knowing the concrete tool.
    class IComponentVisualizerHost
    {
    public:

        virtual ~IComponentVisualizerHost() = default;

        virtual void BeginVisualizerTransaction(ECS::FEntity Entity, CStruct* ComponentType) = 0;
        virtual void EndVisualizerTransaction(FName Label) = 0;
        virtual void MarkVisualizerSceneDirty() = 0;
    };

    // Handle hover, drag and sub-element selection, all of which must survive between frames.
    struct FVisualizerInteractionState
    {
        uint64 ActiveKey = 0;
        ECS::FEntity ActiveEntity = ECS::NullEntity;
        CStruct* ActiveComponentType = nullptr;

        FVector3 GrabAnchor = FVector3(0.0f);
        FVector3 GrabAxis = FVector3(0.0f);
        FVector3 GrabPlaneNormal = FVector3(0.0f);
        FVector3 GrabOffset = FVector3(0.0f);
        FVector3 LastPosition = FVector3(0.0f);

        bool bTransactionOpen = false;
        FName EditLabel;

        ECS::FEntity SelectedEntity = ECS::NullEntity;
        CStruct* SelectedComponentType = nullptr;
        int32 SelectedSubElement = INDEX_NONE;

        // Resolved at the end of a pass and read by the next one, which is how closest-handle-wins works.
        uint64 HoveredKey = 0;
        uint64 PendingHoveredKey = 0;
        int32  PendingHoveredPriority = -1;
        float  PendingHoveredScore = 0.0f;

        bool bCapturedInput = false;

        // Cleared each pass and re-set by the active handle, so a drag whose handle vanished can be released.
        bool bActiveHandleSeen = false;

        void ResetDrag();
        void ClearSelection();
        void Reset();

        NODISCARD bool IsDragging() const { return ActiveKey != 0; }

        void BeginPass();
        void EndPass();
    };

    // Main-thread immediate-mode drawing and interaction surface for a component visualizer.
    class EDITOR_API FComponentVisualizerContext
    {
    public:

        FComponentVisualizerContext(FVisualizerInteractionState& InState, IComponentVisualizerHost* InHost);

        void SetTarget(ECS::FEntity InEntity, CStruct* InComponentType);

        CWorld*                  World = nullptr;
        ECS::FRegistry*          Registry = nullptr;
        IPrimitiveDrawInterface* PDI = nullptr;
        ImDrawList*              DrawList = nullptr;

        FVisualizerView View;

        bool bViewportHovered = false;
        bool bInputEnabled = false;

        NODISCARD ECS::FEntity GetEntity() const { return Entity; }
        NODISCARD CStruct* GetComponentType() const { return ComponentType; }
        NODISCARD ECS::FRegistry& GetRegistry() const { return *Registry; }

        template<typename T>
        NODISCARD T& Get() const { return Registry->Get<T>(Entity); }

        template<typename T>
        NODISCARD T* TryGet() const { return Registry->TryGet<T>(Entity); }

        template<typename T>
        NODISCARD bool Has() const { return Registry->HasAll<T>(Entity); }

        void Line(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness = 1.5f);
        void DashedLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness = 1.5f, float DashPixels = 6.0f);
        void Circle(const FVector3& Center, float PixelRadius, const FVector4& Color, bool bFilled = true, float Thickness = 1.5f);
        void Polygon(const FVector3* Corners, int32 Count, const FVector4& Fill, const FVector4& Outline, float OutlineThickness = 1.5f);
        void Quad(const FVector3& A, const FVector3& B, const FVector3& C, const FVector3& D, const FVector4& Fill, const FVector4& Outline, float OutlineThickness = 1.5f);
        void Text(const FVector3& WorldPos, const FVector4& Color, const char* Format, ...);
        void Label(const FVector3& WorldPos, const FVector4& Color, const char* Format, ...);

        // Dimension line with end ticks and a centered caption, for showing an extent while dragging it.
        void Measurement(const FVector3& Start, const FVector3& End, const FVector4& Color, const char* Format, ...);

        NODISCARD bool IsSubElementSelected(int32 SubElement) const;
        NODISCARD int32 GetSelectedSubElement() const;
        void SelectSubElement(int32 SubElement);
        void ClearSubElementSelection();

        // Screen-space grab, dragged on the plane facing the camera.
        FVisualizerHandleResult PointHandle(uint32 ID, const FVector3& WorldPos, const FVisualizerHandleStyle& Style = FVisualizerHandleStyle());

        // Screen-space grab, dragged along Axis. ScalarDelta is the signed distance moved along it.
        FVisualizerHandleResult AxisHandle(uint32 ID, const FVector3& WorldPos, const FVector3& Axis, const FVisualizerHandleStyle& Style = FVisualizerHandleStyle());

        // Screen-space grab, dragged on the plane through World with the given normal.
        FVisualizerHandleResult PlaneHandle(uint32 ID, const FVector3& WorldPos, const FVector3& Normal, const FVisualizerHandleStyle& Style = FVisualizerHandleStyle());

        // Dot at the center of a quad that drags the quad along its own normal. Back faces are skipped.
        FVisualizerHandleResult FaceHandle(uint32 ID, const FVector3& Center, const FVector3& Normal, const FVector3& HalfU, const FVector3& HalfV, const FVisualizerHandleStyle& Style = FVisualizerHandleStyle());

        // Three axis arms plus a center dot, giving axis-constrained translation without ImGuizmo.
        FVisualizerHandleResult TranslateHandle(uint32 ID, const FVector3& WorldPos, const FVisualizerHandleStyle& Style = FVisualizerHandleStyle());

        // True once any handle this pass reported hover or drag, so a click can fall through when it is false.
        NODISCARD bool IsInteracting() const;

        // Floating ImGui panel anchored to a world position; hosts real widgets. False when off screen.
        bool BeginPanel(const char* ID, const FVector3& WorldAnchor, ImVec2 PixelOffset = ImVec2(18.0f, -10.0f));
        void EndPanel();

        // Names the undo entry the framework commits when the current drag releases.
        void NameEdit(FName Label);

        // Brackets an edit made by a panel widget rather than a handle drag.
        void BeginEdit(FName Label);
        void EndEdit();

        void MarkDirty();

        // Resolves hover for the next pass and closes any interaction the pass left open.
        void FinishPass();

    private:

        enum class EHandleConstraint : uint8
        {
            Screen,
            Axis,
            Plane,
        };

        struct FHandleDesc
        {
            uint32 ID = 0;
            FVector3 Position = FVector3(0.0f);
            FVector3 Axis = FVector3(0.0f);
            FVector3 Normal = FVector3(0.0f);
            FVector3 HalfU = FVector3(0.0f);
            FVector3 HalfV = FVector3(0.0f);
            EHandleConstraint Constraint = EHandleConstraint::Screen;
            bool bFaceSurface = false;

            // Separates the sub-handles of a compound widget from a plain handle sharing the same ID.
            uint32 Salt = 0;
            const FVisualizerHandleStyle* Style = nullptr;
        };

        FVisualizerHandleResult ProcessHandle(const FHandleDesc& Desc);

        NODISCARD uint64 MakeKey(uint32 ID, uint32 Salt) const;

        NODISCARD bool SolveConstraint(const FHandleDesc& Desc, FVector3& OutPosition) const;

        void DrawHandleGlyph(const FHandleDesc& Desc, const ImVec2& Screen, const FVector4& Color) const;

        void DrawFaceSurface(const FHandleDesc& Desc, const FVector4& Color, bool bHot, bool bSelected);

        void BeginDrag(const FHandleDesc& Desc, uint64 Key);
        void EndDrag();

        void OfferHover(uint64 Key, int32 Priority, float Score) const;

        FVisualizerInteractionState& State;
        IComponentVisualizerHost*    Host = nullptr;

        ECS::FEntity Entity = ECS::NullEntity;
        CStruct*     ComponentType = nullptr;

        bool bPanelOpen = false;
    };
}
