#pragma once

#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Containers/Array.h"
#include "Containers/Function.h"
#include "imgui.h"

namespace Lumina
{
    /** Reusable ImGui curve editor drawn straight into a draw list. Holds view and interaction state
     *  only; the SKeyedCurve it edits is owned by the caller, so tools can embed it anywhere. */
    class FCurveEditorWidget
    {
    public:

        void SetCurve(SKeyedCurve* InCurve);
        SKeyedCurve* GetCurve() const { return Curve; }

        /** Invoked once per frame in which any mutation happened. */
        void SetOnModified(const TFunction<void()>& InCallback) { OnModified = InCallback; }

        /** Toolbar strip plus a canvas filling the rest of the content region. */
        void Draw(const char* ID);

        /** Vertical marker drawn across the canvas, for tools that have a playhead of their own. */
        void SetTimeMarker(float InTime) { TimeMarker = InTime; bShowTimeMarker = true; }
        void ClearTimeMarker() { bShowTimeMarker = false; }

        void FrameAll();
        void FrameSelection();

    private:

        enum class EDragMode : uint8
        {
            None,
            Pan,
            Keys,
            BoxSelect,
            Tangent,
        };

        ImVec2 CurveToScreen(float InTime, float InValue) const;
        ImVec2 ScreenToCurve(const ImVec2& InScreen) const;
        float GetPixelsPerTime() const;
        float GetPixelsPerValue() const;
        ImVec2 GetTangentHandlePos(int32 Index, bool bLeave) const;

        void DrawToolbar();
        void DrawGrid(ImDrawList* DrawList) const;
        void DrawCurve(ImDrawList* DrawList) const;
        void DrawTangents(ImDrawList* DrawList) const;
        void DrawKeys(ImDrawList* DrawList) const;
        void DrawOverlay(ImDrawList* DrawList) const;
        void DrawContextMenu();

        void HandleInput(bool bHovered);
        void UpdateHover();
        void UpdateKeyDrag();
        void UpdateTangentDrag();
        void ResortAndRemapSelection();

        void FrameRange(float MinTime, float MaxTime, float MinValue, float MaxValue);
        void ZoomView(const ImVec2& Anchor, float Scale, bool bZoomX, bool bZoomY);
        void ClampView();

        bool IsSelected(int32 Index) const;
        void ToggleSelection(int32 Index);
        void SetSelection(int32 Index);
        void SelectNone();
        void BeginKeyDrag();

        void DeleteSelected();
        void SetSelectionInterpMode(ECurveInterpMode Mode, bool bBroken);
        void FlattenSelectedTangents();
        int32 AddKeyAt(float InTime, float InValue);

        float SnapTime(float InTime) const;
        float SnapValue(float InValue) const;
        void NotifyModified();

    private:

        SKeyedCurve*        Curve = nullptr;
        TFunction<void()>   OnModified;

        // Visible window in curve space.
        float               ViewMinTime = -0.1f;
        float               ViewMaxTime = 1.1f;
        float               ViewMinValue = -0.2f;
        float               ViewMaxValue = 1.2f;
        bool                bViewInitialized = false;

        ImVec2              CanvasMin = ImVec2(0.0f, 0.0f);
        ImVec2              CanvasMax = ImVec2(0.0f, 0.0f);
        ImVec2              MouseCurvePos = ImVec2(0.0f, 0.0f);

        TVector<int32>      Selection;
        TVector<float>      DragStartTimes;
        TVector<float>      DragStartValues;

        EDragMode           DragMode = EDragMode::None;
        bool                bDragMoved = false;
        bool                bDirtyThisFrame = false;
        ImVec2              DragStartScreen = ImVec2(0.0f, 0.0f);
        ImVec2              DragStartCurve = ImVec2(0.0f, 0.0f);

        int32               HoveredKey = INDEX_NONE;
        int32               HoveredTangentKey = INDEX_NONE;
        bool                bHoveredTangentLeave = false;
        int32               TangentDragKey = INDEX_NONE;
        bool                bTangentDragLeave = false;
        int32               ContextKey = INDEX_NONE;

        float               TimeMarker = 0.0f;
        bool                bShowTimeMarker = false;

        bool                bSnapTime = false;
        bool                bSnapValue = false;
        float               TimeSnapStep = 0.1f;
        float               ValueSnapStep = 0.1f;
    };
}
