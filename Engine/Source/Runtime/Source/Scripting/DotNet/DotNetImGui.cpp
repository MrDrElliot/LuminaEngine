#include "RuntimePCH.h"

#include "Containers/String.h"
#include "Core/Threading/Thread.h"
#include "Memory/Memcpy.h"
#include "Scripting/DotNet/DotNetExport.h"

#include <imgui.h>

#if USING(WITH_DEV_UI)
#include "Renderer/RenderManager.h"
#include "Tools/UI/ImGui/ImGuiRenderer.h"
#endif

// The managed ImGui surface. Every export is a no-op off the game thread, outside a frame, and wherever the developer UI is compiled out.

namespace
{
    using namespace Lumina;

    bool CanDraw()
    {
        #if USING(WITH_DEV_UI)
        if (!Threading::IsMainThread())
        {
            return false;
        }
        FRenderManager* Manager = TryRender();
        const IImGuiRenderer* Renderer = Manager != nullptr ? Manager->GetImGuiRenderer() : nullptr;
        return Renderer != nullptr && Renderer->IsFrameOpen();
        #else
        return false;
        #endif
    }

    // ImGui takes NUL-terminated text, while a managed string arrives as UTF-8 bytes and a length.
    class FLabel
    {
    public:

        FLabel(const char* Utf8, int32 Length)
        {
            if (Utf8 == nullptr || Length <= 0)
            {
                Inline[0] = '\0';
                Text = Inline;
            }
            else if (Length < kInlineCapacity)
            {
                Memory::Memcpy(Inline, Utf8, (size_t)Length);
                Inline[Length] = '\0';
                Text = Inline;
            }
            else
            {
                Heap = FStringView(Utf8, (size_t)Length);
                Text = *Heap;
            }
        }
        LE_NO_COPYMOVE(FLabel);

        operator const char*() const { return Text; }

        // Null for an empty string, which BeginPopupContextItem reads as the last item's own id.
        const char* OrNull() const { return Text[0] == '\0' ? nullptr : Text; }

    private:

        static constexpr int32 kInlineCapacity = 256;

        char        Inline[kInlineCapacity];
        FString     Heap;
        const char* Text = nullptr;
    };

    // An empty managed string can arrive as a null pointer, which ImGui would measure with strlen.
    void TextRange(const char* Text, int32 Length)
    {
        ImGui::TextUnformatted(Length > 0 ? Text : "", Length > 0 ? Text + Length : nullptr);
    }

    // The format a managed caller left empty falls back to ImGui's own default for that widget.
    const char* FormatOr(const FLabel& Format, const char* Default)
    {
        const char* Text = Format;
        return Text[0] == '\0' ? Default : Text;
    }
}

#define LUMINA_IMGUI_GUARD(...) if (!CanDraw()) { return __VA_ARGS__; }

//~ Windows

LUMINA_DOTNET_EXPORT(uint8, ImGui_Begin)(const char* Name, int32 NameLen, uint8* Open, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::Begin(FLabel(Name, NameLen), reinterpret_cast<bool*>(Open), Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_End)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::End();
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_BeginChild)(const char* Id, int32 IdLen, float Width, float Height, int32 ChildFlags, int32 WindowFlags)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::BeginChild(FLabel(Id, IdLen), ImVec2(Width, Height), ChildFlags, WindowFlags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_EndChild)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::EndChild();
}

LUMINA_DOTNET_EXPORT(void, ImGui_SetNextWindowPos)(float X, float Y, int32 Cond, float PivotX, float PivotY)
{
    LUMINA_IMGUI_GUARD()
    // The editor's windows live in desktop coordinates, so a script's position is taken from the main window's corner in both.
    const ImVec2 Origin = ImGui::GetMainViewport()->Pos;
    ImGui::SetNextWindowPos(ImVec2(Origin.x + X, Origin.y + Y), Cond, ImVec2(PivotX, PivotY));
}

LUMINA_DOTNET_EXPORT(void, ImGui_SetNextWindowSize)(float Width, float Height, int32 Cond)
{
    LUMINA_IMGUI_GUARD()
    ImGui::SetNextWindowSize(ImVec2(Width, Height), Cond);
}

LUMINA_DOTNET_EXPORT(void, ImGui_SetNextWindowCollapsed)(uint8 bCollapsed, int32 Cond)
{
    LUMINA_IMGUI_GUARD()
    ImGui::SetNextWindowCollapsed(bCollapsed != 0, Cond);
}

LUMINA_DOTNET_EXPORT(void, ImGui_SetNextWindowBgAlpha)(float Alpha)
{
    LUMINA_IMGUI_GUARD()
    ImGui::SetNextWindowBgAlpha(Alpha);
}

LUMINA_DOTNET_EXPORT(void, ImGui_ShowDemoWindow)(uint8* Open)
{
    LUMINA_IMGUI_GUARD()
    ImGui::ShowDemoWindow(reinterpret_cast<bool*>(Open));
}

//~ Layout

LUMINA_DOTNET_EXPORT(void, ImGui_Separator)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::Separator();
}

LUMINA_DOTNET_EXPORT(void, ImGui_SeparatorText)(const char* Text, int32 TextLen)
{
    LUMINA_IMGUI_GUARD()
    ImGui::SeparatorText(FLabel(Text, TextLen));
}

LUMINA_DOTNET_EXPORT(void, ImGui_SameLine)(float OffsetFromStart, float Spacing)
{
    LUMINA_IMGUI_GUARD()
    ImGui::SameLine(OffsetFromStart, Spacing);
}

LUMINA_DOTNET_EXPORT(void, ImGui_NewLine)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::NewLine();
}

LUMINA_DOTNET_EXPORT(void, ImGui_Spacing)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::Spacing();
}

LUMINA_DOTNET_EXPORT(void, ImGui_Dummy)(float Width, float Height)
{
    LUMINA_IMGUI_GUARD()
    ImGui::Dummy(ImVec2(Width, Height));
}

LUMINA_DOTNET_EXPORT(void, ImGui_Indent)(float Width)
{
    LUMINA_IMGUI_GUARD()
    ImGui::Indent(Width);
}

LUMINA_DOTNET_EXPORT(void, ImGui_Unindent)(float Width)
{
    LUMINA_IMGUI_GUARD()
    ImGui::Unindent(Width);
}

LUMINA_DOTNET_EXPORT(void, ImGui_BeginGroup)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::BeginGroup();
}

LUMINA_DOTNET_EXPORT(void, ImGui_EndGroup)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::EndGroup();
}

LUMINA_DOTNET_EXPORT(void, ImGui_SetNextItemWidth)(float Width)
{
    LUMINA_IMGUI_GUARD()
    ImGui::SetNextItemWidth(Width);
}

LUMINA_DOTNET_EXPORT(void, ImGui_PushItemWidth)(float Width)
{
    LUMINA_IMGUI_GUARD()
    ImGui::PushItemWidth(Width);
}

LUMINA_DOTNET_EXPORT(void, ImGui_PopItemWidth)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::PopItemWidth();
}

LUMINA_DOTNET_EXPORT(void, ImGui_AlignTextToFramePadding)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::AlignTextToFramePadding();
}

LUMINA_DOTNET_EXPORT(void, ImGui_GetContentRegionAvail)(float* OutWidth, float* OutHeight)
{
    ImVec2 Avail(0.0f, 0.0f);
    if (CanDraw())
    {
        Avail = ImGui::GetContentRegionAvail();
    }
    *OutWidth = Avail.x;
    *OutHeight = Avail.y;
}

LUMINA_DOTNET_EXPORT(void, ImGui_GetDisplaySize)(float* OutWidth, float* OutHeight)
{
    ImVec2 Size(0.0f, 0.0f);
    if (CanDraw())
    {
        Size = ImGui::GetIO().DisplaySize;
    }
    *OutWidth = Size.x;
    *OutHeight = Size.y;
}

//~ Text

LUMINA_DOTNET_EXPORT(void, ImGui_Text)(const char* Text, int32 TextLen)
{
    LUMINA_IMGUI_GUARD()
    TextRange(Text, TextLen);
}

LUMINA_DOTNET_EXPORT(void, ImGui_TextColored)(float R, float G, float B, float A, const char* Text, int32 TextLen)
{
    LUMINA_IMGUI_GUARD()
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(R, G, B, A));
    TextRange(Text, TextLen);
    ImGui::PopStyleColor();
}

LUMINA_DOTNET_EXPORT(void, ImGui_TextDisabled)(const char* Text, int32 TextLen)
{
    LUMINA_IMGUI_GUARD()
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    TextRange(Text, TextLen);
    ImGui::PopStyleColor();
}

LUMINA_DOTNET_EXPORT(void, ImGui_TextWrapped)(const char* Text, int32 TextLen)
{
    LUMINA_IMGUI_GUARD()
    ImGui::PushTextWrapPos(0.0f);
    TextRange(Text, TextLen);
    ImGui::PopTextWrapPos();
}

LUMINA_DOTNET_EXPORT(void, ImGui_LabelText)(const char* Label, int32 LabelLen, const char* Text, int32 TextLen)
{
    LUMINA_IMGUI_GUARD()
    ImGui::LabelText(FLabel(Label, LabelLen), "%s", static_cast<const char*>(FLabel(Text, TextLen)));
}

LUMINA_DOTNET_EXPORT(void, ImGui_BulletText)(const char* Text, int32 TextLen)
{
    LUMINA_IMGUI_GUARD()
    ImGui::Bullet();
    TextRange(Text, TextLen);
}

//~ Widgets

LUMINA_DOTNET_EXPORT(uint8, ImGui_Button)(const char* Label, int32 LabelLen, float Width, float Height)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::Button(FLabel(Label, LabelLen), ImVec2(Width, Height)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_SmallButton)(const char* Label, int32 LabelLen)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::SmallButton(FLabel(Label, LabelLen)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_Checkbox)(const char* Label, int32 LabelLen, uint8* Value)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::Checkbox(FLabel(Label, LabelLen), reinterpret_cast<bool*>(Value)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_RadioButton)(const char* Label, int32 LabelLen, uint8 bActive)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::RadioButton(FLabel(Label, LabelLen), bActive != 0) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_ProgressBar)(float Fraction, float Width, float Height, const char* Overlay, int32 OverlayLen)
{
    LUMINA_IMGUI_GUARD()
    const FLabel Text(Overlay, OverlayLen);
    ImGui::ProgressBar(Fraction, ImVec2(Width, Height), Text.OrNull());
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_SliderFloatN)(const char* Label, int32 LabelLen, float* Values, int32 Components, float Min, float Max, const char* Format, int32 FormatLen, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    const FLabel Fmt(Format, FormatLen);
    return ImGui::SliderScalarN(FLabel(Label, LabelLen), ImGuiDataType_Float, Values, Components, &Min, &Max, FormatOr(Fmt, "%.3f"), Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_SliderInt)(const char* Label, int32 LabelLen, int32* Value, int32 Min, int32 Max, const char* Format, int32 FormatLen, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    const FLabel Fmt(Format, FormatLen);
    return ImGui::SliderInt(FLabel(Label, LabelLen), Value, Min, Max, FormatOr(Fmt, "%d"), Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_DragFloatN)(const char* Label, int32 LabelLen, float* Values, int32 Components, float Speed, float Min, float Max, const char* Format, int32 FormatLen, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    const FLabel Fmt(Format, FormatLen);
    return ImGui::DragScalarN(FLabel(Label, LabelLen), ImGuiDataType_Float, Values, Components, Speed, &Min, &Max, FormatOr(Fmt, "%.3f"), Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_DragInt)(const char* Label, int32 LabelLen, int32* Value, float Speed, int32 Min, int32 Max, const char* Format, int32 FormatLen, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    const FLabel Fmt(Format, FormatLen);
    return ImGui::DragInt(FLabel(Label, LabelLen), Value, Speed, Min, Max, FormatOr(Fmt, "%d"), Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_InputFloat)(const char* Label, int32 LabelLen, float* Value, float Step, float StepFast, const char* Format, int32 FormatLen, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    const FLabel Fmt(Format, FormatLen);
    return ImGui::InputFloat(FLabel(Label, LabelLen), Value, Step, StepFast, FormatOr(Fmt, "%.3f"), Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_InputInt)(const char* Label, int32 LabelLen, int32* Value, int32 Step, int32 StepFast, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::InputInt(FLabel(Label, LabelLen), Value, Step, StepFast, Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_InputText)(const char* Label, int32 LabelLen, char* Buffer, int32 BufferSize, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::InputText(FLabel(Label, LabelLen), Buffer, (size_t)BufferSize, Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_ColorEdit)(const char* Label, int32 LabelLen, float* Color, uint8 bWithAlpha, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    const FLabel Text(Label, LabelLen);
    const bool bChanged = bWithAlpha != 0 ? ImGui::ColorEdit4(Text, Color, Flags) : ImGui::ColorEdit3(Text, Color, Flags);
    return bChanged ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_Combo)(const char* Label, int32 LabelLen, int32* Current, const char* Items, int32 ItemsLen, int32 MaxVisibleItems)
{
    LUMINA_IMGUI_GUARD(0)
    // The managed side separates items with NULs, and ImGui reads the list as ending at a double NUL.
    FString Separated(Items, (size_t)ItemsLen);
    Separated.AppendChar('\0');
    Separated.AppendChar('\0');
    return ImGui::Combo(FLabel(Label, LabelLen), Current, *Separated, MaxVisibleItems) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_Selectable)(const char* Label, int32 LabelLen, uint8 bSelected, int32 Flags, float Width, float Height)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::Selectable(FLabel(Label, LabelLen), bSelected != 0, Flags, ImVec2(Width, Height)) ? 1 : 0;
}

//~ Trees

LUMINA_DOTNET_EXPORT(uint8, ImGui_TreeNodeEx)(const char* Label, int32 LabelLen, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::TreeNodeEx(FLabel(Label, LabelLen), Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_TreePop)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::TreePop();
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_CollapsingHeader)(const char* Label, int32 LabelLen, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::CollapsingHeader(FLabel(Label, LabelLen), Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_SetNextItemOpen)(uint8 bOpen, int32 Cond)
{
    LUMINA_IMGUI_GUARD()
    ImGui::SetNextItemOpen(bOpen != 0, Cond);
}

//~ Tables

LUMINA_DOTNET_EXPORT(uint8, ImGui_BeginTable)(const char* Id, int32 IdLen, int32 Columns, int32 Flags, float OuterWidth, float OuterHeight, float InnerWidth)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::BeginTable(FLabel(Id, IdLen), Columns, Flags, ImVec2(OuterWidth, OuterHeight), InnerWidth) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_EndTable)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::EndTable();
}

LUMINA_DOTNET_EXPORT(void, ImGui_TableNextRow)(int32 RowFlags, float MinHeight)
{
    LUMINA_IMGUI_GUARD()
    ImGui::TableNextRow(RowFlags, MinHeight);
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_TableNextColumn)()
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::TableNextColumn() ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_TableSetColumnIndex)(int32 Column)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::TableSetColumnIndex(Column) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_TableSetupColumn)(const char* Label, int32 LabelLen, int32 Flags, float WidthOrWeight)
{
    LUMINA_IMGUI_GUARD()
    ImGui::TableSetupColumn(FLabel(Label, LabelLen), Flags, WidthOrWeight);
}

LUMINA_DOTNET_EXPORT(void, ImGui_TableSetupScrollFreeze)(int32 Columns, int32 Rows)
{
    LUMINA_IMGUI_GUARD()
    ImGui::TableSetupScrollFreeze(Columns, Rows);
}

LUMINA_DOTNET_EXPORT(void, ImGui_TableHeadersRow)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::TableHeadersRow();
}

//~ Tooltips and popups

LUMINA_DOTNET_EXPORT(void, ImGui_SetTooltip)(const char* Text, int32 TextLen)
{
    LUMINA_IMGUI_GUARD()
    ImGui::SetTooltip("%s", static_cast<const char*>(FLabel(Text, TextLen)));
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_BeginTooltip)()
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::BeginTooltip() ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_BeginItemTooltip)()
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::BeginItemTooltip() ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_EndTooltip)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::EndTooltip();
}

LUMINA_DOTNET_EXPORT(void, ImGui_OpenPopup)(const char* Id, int32 IdLen, int32 Flags)
{
    LUMINA_IMGUI_GUARD()
    ImGui::OpenPopup(FLabel(Id, IdLen), Flags);
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_BeginPopup)(const char* Id, int32 IdLen, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::BeginPopup(FLabel(Id, IdLen), Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_BeginPopupModal)(const char* Name, int32 NameLen, uint8* Open, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::BeginPopupModal(FLabel(Name, NameLen), reinterpret_cast<bool*>(Open), Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_BeginPopupContextItem)(const char* Id, int32 IdLen, int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    const FLabel Text(Id, IdLen);
    return ImGui::BeginPopupContextItem(Text.OrNull(), Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_EndPopup)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::EndPopup();
}

LUMINA_DOTNET_EXPORT(void, ImGui_CloseCurrentPopup)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::CloseCurrentPopup();
}

//~ Menus

LUMINA_DOTNET_EXPORT(uint8, ImGui_BeginMenuBar)()
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::BeginMenuBar() ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_EndMenuBar)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::EndMenuBar();
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_BeginMainMenuBar)()
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::BeginMainMenuBar() ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_EndMainMenuBar)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::EndMainMenuBar();
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_BeginMenu)(const char* Label, int32 LabelLen, uint8 bEnabled)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::BeginMenu(FLabel(Label, LabelLen), bEnabled != 0) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, ImGui_EndMenu)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::EndMenu();
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_MenuItem)(const char* Label, int32 LabelLen, const char* Shortcut, int32 ShortcutLen, uint8 bSelected, uint8 bEnabled)
{
    LUMINA_IMGUI_GUARD(0)
    const FLabel Keys(Shortcut, ShortcutLen);
    return ImGui::MenuItem(FLabel(Label, LabelLen), Keys.OrNull(), bSelected != 0, bEnabled != 0) ? 1 : 0;
}

//~ Plots

LUMINA_DOTNET_EXPORT(void, ImGui_PlotLines)(const char* Label, int32 LabelLen, const float* Values, int32 Count, int32 Offset,
    const char* Overlay, int32 OverlayLen, float ScaleMin, float ScaleMax, float Width, float Height, uint8 bHistogram)
{
    LUMINA_IMGUI_GUARD()
    const FLabel Text(Label, LabelLen);
    const FLabel OverlayText(Overlay, OverlayLen);
    if (bHistogram != 0)
    {
        ImGui::PlotHistogram(Text, Values, Count, Offset, OverlayText.OrNull(), ScaleMin, ScaleMax, ImVec2(Width, Height));
        return;
    }
    ImGui::PlotLines(Text, Values, Count, Offset, OverlayText.OrNull(), ScaleMin, ScaleMax, ImVec2(Width, Height));
}

//~ Item and window queries

LUMINA_DOTNET_EXPORT(uint8, ImGui_IsItemHovered)(int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::IsItemHovered(Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_IsItemClicked)(int32 Button)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::IsItemClicked(Button) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_IsItemActive)()
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::IsItemActive() ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_IsWindowHovered)(int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::IsWindowHovered(Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_IsWindowFocused)(int32 Flags)
{
    LUMINA_IMGUI_GUARD(0)
    return ImGui::IsWindowFocused(Flags) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(float, ImGui_GetFramerate)()
{
    LUMINA_IMGUI_GUARD(0.0f)
    return ImGui::GetIO().Framerate;
}

//~ Ids and style

LUMINA_DOTNET_EXPORT(void, ImGui_PushID)(const char* Id, int32 IdLen)
{
    LUMINA_IMGUI_GUARD()
    ImGui::PushID(static_cast<const char*>(FLabel(Id, IdLen)));
}

LUMINA_DOTNET_EXPORT(void, ImGui_PushIDInt)(int32 Id)
{
    LUMINA_IMGUI_GUARD()
    ImGui::PushID(Id);
}

LUMINA_DOTNET_EXPORT(void, ImGui_PopID)()
{
    LUMINA_IMGUI_GUARD()
    ImGui::PopID();
}

LUMINA_DOTNET_EXPORT(void, ImGui_PushStyleColor)(int32 Index, float R, float G, float B, float A)
{
    LUMINA_IMGUI_GUARD()
    ImGui::PushStyleColor(Index, ImVec4(R, G, B, A));
}

LUMINA_DOTNET_EXPORT(void, ImGui_PopStyleColor)(int32 Count)
{
    LUMINA_IMGUI_GUARD()
    ImGui::PopStyleColor(Count);
}

LUMINA_DOTNET_EXPORT(void, ImGui_PushStyleVarFloat)(int32 Index, float Value)
{
    LUMINA_IMGUI_GUARD()
    ImGui::PushStyleVar(Index, Value);
}

LUMINA_DOTNET_EXPORT(void, ImGui_PushStyleVarVec2)(int32 Index, float X, float Y)
{
    LUMINA_IMGUI_GUARD()
    ImGui::PushStyleVar(Index, ImVec2(X, Y));
}

LUMINA_DOTNET_EXPORT(void, ImGui_PopStyleVar)(int32 Count)
{
    LUMINA_IMGUI_GUARD()
    ImGui::PopStyleVar(Count);
}

LUMINA_DOTNET_EXPORT(uint8, ImGui_IsAvailable)()
{
    return CanDraw() ? 1 : 0;
}
