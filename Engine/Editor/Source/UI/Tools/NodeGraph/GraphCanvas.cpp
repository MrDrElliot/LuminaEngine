#include "GraphCanvas.h"

#include "Core/Math/Math.h"

namespace Lumina
{
    namespace
    {
        // Marks where canvas commands begin, so End never moves a clip rect drawn before it. Removed at End, never executed.
        void CanvasSentinelCallback(const ImDrawList*, const ImDrawCmd*)
        {
        }

        // Quarter-octave steps keep a smooth zoom from baking the font at a new size every frame.
        float QuantizeFontDensity(float Density)
        {
            return Math::Exp2(Math::Round(Math::Log2(Math::Clamp(Density, 0.25f, 4.0f)) * 4.0f) / 4.0f);
        }
    }

    void FGraphCanvas::Begin()
    {
        DrawList = ImGui::GetWindowDrawList();

        ImGuiIO& IO = ImGui::GetIO();
        SavedMousePos     = IO.MousePos;
        SavedMousePosPrev = IO.MousePosPrev;
        for (int32 Button = 0; Button < ImGuiMouseButton_COUNT; ++Button)
        {
            SavedMouseClickedPos[Button] = IO.MouseClickedPos[Button];
        }

        ImGuiWindow*   Window   = ImGui::GetCurrentWindow();
        ImGuiViewport* Viewport = ImGui::GetWindowViewport();
        SavedWindowPos    = Window->Pos;
        SavedCursorMaxPos = Window->DC.CursorMaxPos;
        SavedViewportPos  = Viewport->Pos;
        SavedViewportSize = Viewport->Size;
        SavedWorkPos      = Viewport->WorkPos;
        SavedWorkSize     = Viewport->WorkSize;
        SavedFringeScale  = DrawList->_FringeScale;
        SavedFontDensity  = ImGui::GetFontRasterizerDensity();

        const ImRect ScreenRect = GetScreenRect();
        ImGui::PushClipRect(ScreenRect.Min, ScreenRect.Max, true);
        const ImVec4 ScreenClip = DrawList->_ClipRectStack.back();
        ImGui::PopClipRect();

        SentinelCommand = DrawList->CmdBuffer.Size;
        DrawList->AddCallback(CanvasSentinelCallback, nullptr);
        FirstVertex = DrawList->VtxBuffer.Size;

        Window->Pos      = ImVec2(0.0f, 0.0f);
        Viewport->Pos    = ScreenToCanvas(SavedViewportPos);
        Viewport->Size   = SavedViewportSize / Zoom;
        Viewport->WorkPos  = ScreenToCanvas(SavedWorkPos);
        Viewport->WorkSize = SavedWorkSize / Zoom;

        ImGui::PushClipRect(ScreenToCanvas(ImVec2(ScreenClip.x, ScreenClip.y)), ScreenToCanvas(ImVec2(ScreenClip.z, ScreenClip.w)), false);

        auto ToLocal = [this](ImVec2 Point)
        {
            return ImGui::IsMousePosValid(&Point) ? ScreenToCanvas(Point) : Point;
        };
        IO.MousePos     = ToLocal(SavedMousePos);
        IO.MousePosPrev = ToLocal(SavedMousePosPrev);
        for (int32 Button = 0; Button < ImGuiMouseButton_COUNT; ++Button)
        {
            IO.MouseClickedPos[Button] = ToLocal(SavedMouseClickedPos[Button]);
        }

        // Fringes stay one screen pixel wide and glyphs are rasterized for the size they land on screen.
        DrawList->_FringeScale = SavedFringeScale / Zoom;
        ImGui::SetFontRasterizerDensity(QuantizeFontDensity(SavedFontDensity * Zoom));

        bInLocalSpace = true;
    }

    void FGraphCanvas::End()
    {
        ImDrawVert* Vertices = DrawList->VtxBuffer.Data;
        for (int32 Index = FirstVertex; Index < DrawList->VtxBuffer.Size; ++Index)
        {
            Vertices[Index].pos = CanvasToScreen(Vertices[Index].pos);
        }

        for (int32 Index = SentinelCommand + 1; Index < DrawList->CmdBuffer.Size; ++Index)
        {
            ImVec4& Clip = DrawList->CmdBuffer[Index].ClipRect;
            const ImVec2 Min = CanvasToScreen(ImVec2(Clip.x, Clip.y));
            const ImVec2 Max = CanvasToScreen(ImVec2(Clip.z, Clip.w));
            Clip = ImVec4(Min.x, Min.y, Max.x, Max.y);
        }

        if (SentinelCommand < DrawList->CmdBuffer.Size && DrawList->CmdBuffer[SentinelCommand].UserCallback == CanvasSentinelCallback)
        {
            DrawList->CmdBuffer.erase(DrawList->CmdBuffer.Data + SentinelCommand);
        }

        ImGui::PopClipRect();

        DrawList->_FringeScale = SavedFringeScale;
        ImGui::SetFontRasterizerDensity(SavedFontDensity);

        ImGuiIO& IO = ImGui::GetIO();
        IO.MousePos     = SavedMousePos;
        IO.MousePosPrev = SavedMousePosPrev;
        for (int32 Button = 0; Button < ImGuiMouseButton_COUNT; ++Button)
        {
            IO.MouseClickedPos[Button] = SavedMouseClickedPos[Button];
        }

        ImGuiWindow*   Window   = ImGui::GetCurrentWindow();
        ImGuiViewport* Viewport = ImGui::GetWindowViewport();
        Window->Pos             = SavedWindowPos;
        Window->DC.CursorMaxPos = SavedCursorMaxPos;
        Viewport->Pos      = SavedViewportPos;
        Viewport->Size     = SavedViewportSize;
        Viewport->WorkPos  = SavedWorkPos;
        Viewport->WorkSize = SavedWorkSize;

        bInLocalSpace = false;
    }
}
