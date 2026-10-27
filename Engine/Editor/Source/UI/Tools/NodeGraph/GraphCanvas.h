#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // A pan and zoom view onto an unbounded plane. Between Begin and End ImGui works in canvas units, so any widget draws and hit-tests correctly at any zoom.
    class EDITOR_API FGraphCanvas
    {
    public:

        // Where the canvas lands on screen, set before Begin so the view can be updated against it.
        void SetScreenRect(const ImRect& InScreenRect) { ScreenMin = InScreenRect.Min; ScreenSize = InScreenRect.GetSize(); }

        void Begin();
        void End();

        ImVec2 ScreenToCanvas(ImVec2 Point) const { return ImVec2((Point.x - ScreenMin.x) / Zoom + Origin.x, (Point.y - ScreenMin.y) / Zoom + Origin.y); }
        ImVec2 CanvasToScreen(ImVec2 Point) const { return ImVec2((Point.x - Origin.x) * Zoom + ScreenMin.x, (Point.y - Origin.y) * Zoom + ScreenMin.y); }

        float  GetZoom() const { return Zoom; }
        ImVec2 GetOrigin() const { return Origin; }
        ImRect GetScreenRect() const { return ImRect(ScreenMin, ImVec2(ScreenMin.x + ScreenSize.x, ScreenMin.y + ScreenSize.y)); }
        ImRect GetVisibleRect() const { return ImRect(Origin, ImVec2(Origin.x + ScreenSize.x / Zoom, Origin.y + ScreenSize.y / Zoom)); }
        bool   IsInLocalSpace() const { return bInLocalSpace; }

        // ImGui's own mouse is in canvas units between Begin and End, so screen-space gestures read these.
        ImVec2 GetScreenMousePos() const { return bInLocalSpace ? SavedMousePos : ImGui::GetIO().MousePos; }
        ImVec2 GetScreenMouseClickedPos(ImGuiMouseButton Button) const { return bInLocalSpace ? SavedMouseClickedPos[Button] : ImGui::GetIO().MouseClickedPos[Button]; }

        // Origin is the canvas point drawn at the top left of the screen rect.
        void SetView(ImVec2 InOrigin, float InZoom) { Origin = InOrigin; Zoom = InZoom; }

    private:

        ImVec2      ScreenMin;
        ImVec2      ScreenSize;
        ImVec2      Origin;
        float       Zoom = 1.0f;
        bool        bInLocalSpace = false;

        ImDrawList* DrawList = nullptr;
        int32       FirstVertex = 0;
        int32       SentinelCommand = 0;

        ImVec2      SavedMousePos;
        ImVec2      SavedMousePosPrev;
        ImVec2      SavedMouseClickedPos[ImGuiMouseButton_COUNT];
        ImVec2      SavedWindowPos;
        ImVec2      SavedViewportPos;
        ImVec2      SavedViewportSize;
        ImVec2      SavedWorkPos;
        ImVec2      SavedWorkSize;
        ImVec2      SavedCursorMaxPos;
        float       SavedFringeScale = 1.0f;
        float       SavedFontDensity = 1.0f;
    };
}
