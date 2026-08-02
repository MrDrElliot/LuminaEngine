#pragma once

#include "Config/EngineSettings.h"
#include "Core/Object/ObjectCore.h"
#include "imgui.h"

// Central editor color palette. Reference these semantic colors instead of hardcoding ImVec4s so the
// editor stays consistent and users can theme it -- the values live on CEditorColorSettings and persist
// with the editor preferences. Each accessor reads the live setting, so edits apply immediately (the ImGui
// renderer re-derives the global style on change).
namespace Lumina::EditorColors
{
    inline ImVec4 ToImVec4(const FVector4& C) { return ImVec4(C.x, C.y, C.z, C.w); }

    inline const CEditorColorSettings& Palette() { return *GetDefault<CEditorColorSettings>(); }

    inline ImVec4 Accent()        { return ToImVec4(Palette().Accent); }
    inline ImVec4 AccentAlt()     { return ToImVec4(Palette().AccentAlt); }
    inline ImVec4 Success()       { return ToImVec4(Palette().Success); }
    inline ImVec4 Warning()       { return ToImVec4(Palette().Warning); }
    inline ImVec4 Danger()        { return ToImVec4(Palette().Danger); }
    inline ImVec4 SectionHeader() { return ToImVec4(Palette().SectionHeader); }
    inline ImVec4 EntityIcon()    { return ToImVec4(Palette().EntityIcon); }
    inline ImVec4 TextPrimary()   { return ToImVec4(Palette().TextPrimary); }
    inline ImVec4 TextDim()       { return ToImVec4(Palette().TextDim); }
    inline ImVec4 TextMuted()     { return ToImVec4(Palette().TextMuted); }
    inline ImVec4 WindowBg()      { return ToImVec4(Palette().WindowBg); }
    inline ImVec4 FrameBg()       { return ToImVec4(Palette().FrameBg); }
    inline ImVec4 TitleBg()       { return ToImVec4(Palette().TitleBg); }
    inline ImVec4 Button()        { return ToImVec4(Palette().Button); }
    inline ImVec4 Header()        { return ToImVec4(Palette().Header); }
    inline ImVec4 Border()        { return ToImVec4(Palette().Border); }
    inline ImVec4 PanelBg()       { return ToImVec4(Palette().PanelBg); }
    inline ImVec4 RowBg()         { return ToImVec4(Palette().RowBg); }
    inline ImVec4 RowBgHovered()  { return ToImVec4(Palette().RowBgHovered); }
    inline ImVec4 RowBgActive()   { return ToImVec4(Palette().RowBgActive); }

    // Derivations.
    inline float  Clamp01(float V)                    { return V < 0.0f ? 0.0f : (V > 1.0f ? 1.0f : V); }
    inline ImVec4 WithAlpha(const ImVec4& C, float A) { return ImVec4(C.x, C.y, C.z, A); }
    inline ImVec4 Lighten(const ImVec4& C, float D)   { return ImVec4(Clamp01(C.x + D), Clamp01(C.y + D), Clamp01(C.z + D), C.w); }
    inline ImU32  U32(const ImVec4& C)                { return ImGui::ColorConvertFloat4ToU32(C); }
}
