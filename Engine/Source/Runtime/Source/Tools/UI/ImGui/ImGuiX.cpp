#include "Containers/StringFormat.h"
#include "RuntimePCH.h"
#include <iterator>
#include "ImGuiX.h"

#include "Core/Object/InstancedStruct.h"
#include "ImGuiDesignIcons.h"
#include "ImGuiRenderer.h"
#include "imgui_internal.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Application/Application.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Windows/Window.h"
#include "Paths/Paths.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHITexture.h"

namespace Lumina::ImGuiX
{
    namespace
    {
        float GUIScale = 1.0f;
    }

    float GetUIScale()
    {
        return GUIScale;
    }

    void SetUIScale(float Scale)
    {
        GUIScale = Scale > 0.0f ? Scale : 1.0f;
    }

    namespace Detail
    {
        // Mirrors IMGUI_DISPLAY_* in Includes/ImGuiCommon.slang.
        constexpr uint32 GDisplayModeDirect = 0u;
        constexpr uint32 GDisplayModeHDR    = 1u;

        static void DisplayStateCallback(const ImDrawList*, const ImDrawCmd*)
        {
            // Intentionally empty, since the backend intercepts this by identity before it would run.
        }

        ImDrawCallback GetDisplayStateCallback()
        {
            return &DisplayStateCallback;
        }
    }

    void BeginHDRPreview(ImDrawList* DrawList, float ExposureStops)
    {
        if (DrawList == nullptr)
        {
            return;
        }

        Detail::FImGuiDisplayState State;
        State.DisplayMode = Detail::GDisplayModeHDR;
        State.Exposure    = std::exp2(ExposureStops);

        // A non-zero size makes ImGui copy the payload into the draw list, so it outlives this call.
        DrawList->AddCallback(Detail::GetDisplayStateCallback(), &State, sizeof(State));
    }

    void EndHDRPreview(ImDrawList* DrawList)
    {
        if (DrawList == nullptr)
        {
            return;
        }

        Detail::FImGuiDisplayState State;
        State.DisplayMode = Detail::GDisplayModeDirect;
        DrawList->AddCallback(Detail::GetDisplayStateCallback(), &State, sizeof(State));
    }

    void BeginArrayPreview(ImDrawList* DrawList, uint32 Slice)
    {
        if (DrawList == nullptr)
        {
            return;
        }

        Detail::FImGuiDisplayState State;
        State.bIsArray   = 1;
        State.ArraySlice = Slice;

        // A non-zero size makes ImGui copy the payload into the draw list, so it outlives this call.
        DrawList->AddCallback(Detail::GetDisplayStateCallback(), &State, sizeof(State));
    }

    void EndArrayPreview(ImDrawList* DrawList)
    {
        if (DrawList == nullptr)
        {
            return;
        }

        // Every other ImGui draw depends on the default state, so this pair must not be left unbalanced.
        Detail::FImGuiDisplayState State;
        DrawList->AddCallback(Detail::GetDisplayStateCallback(), &State, sizeof(State));
    }

    void TextTooltip_Internal(FStringView String)
    {
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
        	if (!ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None))
        	{
        		return;
        	}
        	
        	ImGui::TextUnformatted(String.data());
        	ImGui::EndTooltip();
        }
        ImGui::PopStyleVar();
    }

    void WrappedTooltip_Internal(FStringView String)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            if (ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None))
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted(String.data(), String.data() + String.size());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        ImGui::PopStyleVar();
    }

    void HelpMarker(FStringView Help)
    {
        HelpMarkerIcon("(?)", Help);
    }

    void HelpMarkerIcon(const char* Icon, FStringView Help)
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(Icon);
        ImGui::PopStyleColor();
        WrappedTooltip_Internal(Help);
    }

    void TextColoredUnformatted(const ImVec4& Color, const FFixedString& String)
    {
    	ImGui::PushStyleColor(ImGuiCol_Text, Color);
    	ImGui::TextUnformatted(String.c_str());
    	ImGui::PopStyleColor();
    }

    void TextUnformatted(FStringView String)
    {
    	ImGui::TextUnformatted(String.data(), String.data() + String.size());
    }

    FStringView ImGuizmoOpToString(ImGuizmo::OPERATION Op)
    {
    	switch (Op)
    	{
    		case ImGuizmo::OPERATION::TRANSLATE:	return "Translate";
    		case ImGuizmo::OPERATION::ROTATE:		return "Rotate";
    		case ImGuizmo::OPERATION::SCALE:		return "Scale";

    		// ImGuizmo's per-axis and compound flags have no name of their own here.
    		default:								break;
    	}
    	
    	return "";
    }

    bool ButtonEx(char const* pIcon, char const* pLabel, ImVec2 const& size, const ImColor& backgroundColor, const ImColor& iconColor, const ImColor& foregroundColor, bool shouldCenterContents)
    {
    	// Taken from Esoterica.
    	
    	bool wasPressed = false;

        ImU32 HoveredColor = ImGui::ColorConvertFloat4ToU32(backgroundColor.Value * 1.15f);
        ImU32 ActiveColor  = ImGui::ColorConvertFloat4ToU32(backgroundColor.Value * 1.25f);

        if ( pIcon == nullptr || strlen( pIcon ) == 0 )
        {
            ImGui::PushStyleColor( ImGuiCol_Button, (ImVec4) backgroundColor );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, HoveredColor );
            ImGui::PushStyleColor( ImGuiCol_ButtonActive, ActiveColor );
            ImGui::PushStyleColor( ImGuiCol_Text, (ImVec4) foregroundColor );
            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, shouldCenterContents ? ImVec2( 0.5f, 0.5f ) : ImVec2( 0.0f, 0.5f ) );
            wasPressed = ImGui::Button( pLabel, size );
            ImGui::PopStyleColor( 4 );
            ImGui::PopStyleVar();
        }
        else // Icon button
        {
            ImGuiWindow* pWindow = ImGui::GetCurrentWindow();
            if ( pWindow->SkipItems )
            {
                return false;
            }

            char const* pID = nullptr;
            if ( pLabel == nullptr || strlen( pLabel ) == 0 )
            {
                pID = pIcon;
            }
            else
            {
                pID = pLabel;
            }

            ImGuiID const ID = pWindow->GetID( pID );

            ImGuiStyle const& style = ImGui::GetStyle();
            ImVec2 const iconSize = ImGui::CalcTextSize( pIcon, nullptr, true );
            ImVec2 const labelSize = ImGui::CalcTextSize( pLabel, nullptr, true );
            float const spacing = ( iconSize.x > 0 && labelSize.x > 0 ) ? style.ItemSpacing.x : 0.0f;
            float const buttonWidth = labelSize.x + iconSize.x + spacing;
            float const buttonWidthWithFramePadding = buttonWidth + ( style.FramePadding.x * 2.0f );
            float const textHeightMax = Math::Max( iconSize.y, labelSize.y );
            float const buttonHeight = Math::Max( ImGui::GetFrameHeight(), textHeightMax );

            ImVec2 const pos = pWindow->DC.CursorPos;
            ImVec2 const finalButtonSize = ImGui::CalcItemSize( size, buttonWidthWithFramePadding, buttonHeight );

            ImGui::ItemSize( finalButtonSize, 0 );
            ImRect const bb( pos, pos + finalButtonSize );
            if ( !ImGui::ItemAdd( bb, ID ) )
            {
                return false;
            }

            bool hovered, held;
            wasPressed = ImGui::ButtonBehavior( bb, ID, &hovered, &held, 0 );

            ImGui::PushStyleColor( ImGuiCol_Button, (ImVec4) backgroundColor );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, HoveredColor );
            ImGui::PushStyleColor( ImGuiCol_ButtonActive, ActiveColor );
            ImGui::PushStyleColor( ImGuiCol_Text, (ImVec4) foregroundColor );

            ImU32 const color = ImGui::GetColorU32( ( held && hovered ) ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button );
            //ImGui::RenderNavCursor( bb, ID );
            ImGui::RenderFrame( bb.Min, bb.Max, color, true, style.FrameRounding );

            const ImU32 finalIconColor = iconColor;

            // Left-aligning a lone glyph looks ragged across icons with different advance widths.
            if ( shouldCenterContents || labelSize.x <= 0.0f )
            {
                if ( labelSize.x > 0 )
                {
                    ImVec2 const textOffset( ( bb.GetWidth() / 2 ) - ( buttonWidthWithFramePadding / 2 ) + iconSize.x + spacing + style.FramePadding.x, 0 );
                    ImGui::RenderTextClipped( bb.Min + textOffset, bb.Max, pLabel, NULL, &labelSize, ImVec2( 0, 0.5f ), &bb );

                    float const offsetX = textOffset.x - iconSize.x - spacing;
                    float const offsetY = ( ( bb.GetHeight() - textHeightMax ) / 2.0f );
                    ImVec2 const iconOffset( offsetX, offsetY );
                    pWindow->DrawList->AddText( pos + iconOffset, finalIconColor, pIcon );
                }
                else
                {
                    float const offsetX = ( bb.GetWidth() - iconSize.x ) / 2.0f;
                    float const offsetY = ( ( bb.GetHeight() - iconSize.y ) / 2.0f );
                    ImVec2 const iconOffset( offsetX, offsetY );
                    pWindow->DrawList->AddText( pos + iconOffset, finalIconColor, pIcon );
                }
            }
            else
            {
                ImVec2 const textOffset( iconSize.x + spacing + style.FramePadding.x, 0 );
                ImGui::RenderTextClipped( bb.Min + textOffset, bb.Max, pLabel, NULL, &labelSize, ImVec2( 0, 0.5f ), &bb );

                float const iconHeightOffset = ( ( bb.GetHeight() - iconSize.y ) / 2.0f );
                pWindow->DrawList->AddText( pos + ImVec2( style.FramePadding.x, iconHeightOffset ), finalIconColor, pIcon );
            }

            ImGui::PopStyleColor( 4 );
        }

        return wasPressed;
    }

    namespace
    {
        ImU32 LerpColor(ImU32 A, ImU32 B, float T)
        {
            return ImGui::ColorConvertFloat4ToU32(ImLerp(ImGui::ColorConvertU32ToFloat4(A), ImGui::ColorConvertU32ToFloat4(B), ImSaturate(T)));
        }

        // Shared float/int implementation. Capsule track + shaded circular ("sphere") knob.
        bool SliderScalarStyled(const char* Label, ImGuiDataType DataType, void* Value, const void* Min, const void* Max, ESliderFlags Flags, const char* Format, const FSliderStyle* StyleOverride)
        {
            ImGuiWindow* Window = ImGui::GetCurrentWindow();
            if (Window->SkipItems)
            {
                return false;
            }

            ImGuiContext& g = *GImGui;
            const ImGuiStyle& Style = g.Style;
            const ImGuiID ID = Window->GetID(Label);
            const float Scale = GetUIScale();

            const FSliderStyle SS = StyleOverride ? *StyleOverride : FSliderStyle();
            const float KnobRadius = ImMax(2.0f, SS.KnobRadius * Scale);
            const float TrackHeight = ImMax(2.0f, SS.TrackHeight * Scale);
            const bool ReadOnly = EnumHasAnyFlags(Flags, ESliderFlags::ReadOnly);

            const float Width = ImGui::CalcItemWidth();
            const ImVec2 LabelSize = ImGui::CalcTextSize(Label, nullptr, true);
            const float RowHeight = ImMax(KnobRadius * 2.0f, g.FontSize + Style.FramePadding.y * 2.0f);

            const ImVec2 Pos = Window->DC.CursorPos;
            const ImRect FrameBB(Pos, Pos + ImVec2(Width, RowHeight));
            const ImRect TotalBB(FrameBB.Min, FrameBB.Max + ImVec2(LabelSize.x > 0.0f ? Style.ItemInnerSpacing.x + LabelSize.x : 0.0f, 0.0f));

            const bool TempInputAllowed = !ReadOnly;
            ImGui::ItemSize(TotalBB, Style.FramePadding.y);
            if (!ImGui::ItemAdd(TotalBB, ID, &FrameBB, TempInputAllowed ? ImGuiItemFlags_Inputable : 0))
            {
                return false;
            }

            if (Format == nullptr)
            {
                Format = ImGui::DataTypeGetInfo(DataType)->PrintFmt;
            }

            const bool Hovered = ImGui::ItemHoverable(FrameBB, ID, g.LastItemData.ItemFlags);
            bool TempInputActive = TempInputAllowed && ImGui::TempInputIsActive(ID);

            // Activation by click or nav, with Ctrl and click opening the text-input box.
            if (!ReadOnly && !TempInputActive)
            {
                const bool Clicked = Hovered && ImGui::IsMouseClicked(0, ImGuiInputFlags_None, ID);
                const bool MakeActive = (Clicked || g.NavActivateId == ID);
                if (MakeActive && Clicked)
                {
                    ImGui::SetKeyOwner(ImGuiKey_MouseLeft, ID);
                }
                if (MakeActive && ((Clicked && g.IO.KeyCtrl) || (g.NavActivateId == ID && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput))))
                {
                    TempInputActive = true;
                }

                if (MakeActive)
                {
                    memcpy(&g.ActiveIdValueOnActivation, Value, ImGui::DataTypeGetInfo(DataType)->Size);
                }
                if (MakeActive && !TempInputActive)
                {
                    ImGui::SetActiveID(ID, Window);
                    ImGui::SetFocusID(ID, Window);
                    ImGui::FocusWindow(Window);
                    g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
                }
            }

            if (TempInputActive)
            {
                return ImGui::TempInputScalar(FrameBB, ID, Label, DataType, Value, Format, nullptr, nullptr);
            }

            // Forces the usable grab range to match the knob travel, so the cursor tracks the knob exactly.
            bool ValueChanged = false;
            if (!ReadOnly)
            {
                const float SavedGrabMinSize = g.Style.GrabMinSize;
                g.Style.GrabMinSize = ImMax(1.0f, 2.0f * (KnobRadius - 2.0f));
                ImRect GrabBB;
                ValueChanged = ImGui::SliderBehavior(FrameBB, ID, DataType, Value, Min, Max, Format, ImGuiSliderFlags_None, &GrabBB);
                g.Style.GrabMinSize = SavedGrabMinSize;
                if (ValueChanged)
                {
                    ImGui::MarkItemEdited(ID);
                }
            }

            const bool Held = (g.ActiveId == ID);

            // Normalized position for the knob/fill (linear).
            double DMin = 0.0, DMax = 0.0, DVal = 0.0;
            if (DataType == ImGuiDataType_Float)
            {
                DMin = *(const float*)Min; DMax = *(const float*)Max; DVal = *(const float*)Value;
            }
            else
            {
                DMin = (double)*(const int32*)Min; DMax = (double)*(const int32*)Max; DVal = (double)*(const int32*)Value;
            }
            const float T = (DMax != DMin) ? ImSaturate((float)((DVal - DMin) / (DMax - DMin))) : 0.0f;

            const float TrackLeft = FrameBB.Min.x + KnobRadius;
            const float TrackRight = FrameBB.Max.x - KnobRadius;
            const float CenterY = ImFloor((FrameBB.Min.y + FrameBB.Max.y) * 0.5f + 0.5f);
            const float KnobX = TrackLeft + T * (TrackRight - TrackLeft);
            const ImVec2 KnobCenter(KnobX, CenterY);

            ImDrawList* DL = Window->DrawList;
            const float Rounding = TrackHeight * 0.5f;
            const ImVec2 TrackMin(FrameBB.Min.x, CenterY - TrackHeight * 0.5f);
            const ImVec2 TrackMax(FrameBB.Max.x, CenterY + TrackHeight * 0.5f);

            const ImU32 TrackCol = SS.TrackColor ? SS.TrackColor : ImGui::GetColorU32(ImGuiCol_FrameBg);
            const ImU32 FillCol = SS.FillColor ? SS.FillColor : ImGui::GetColorU32(ImGuiCol_SliderGrab);
            const ImU32 FillEndCol = SS.FillColorEnd ? SS.FillColorEnd : ImGui::GetColorU32(ImGuiCol_SliderGrabActive);
            const ImU32 KnobBase = SS.KnobColor ? SS.KnobColor : ImGui::ColorConvertFloat4ToU32(ImVec4(0.93f, 0.94f, 0.96f, 1.0f));
            const ImU32 KnobHovered = SS.KnobColorHovered ? SS.KnobColorHovered : IM_COL32(255, 255, 255, 255);
            const ImU32 KnobCol = (Held || Hovered) ? KnobHovered : KnobBase;

            // Track background.
            DL->AddRectFilled(TrackMin, TrackMax, TrackCol, Rounding);

            // Filled portion up to the knob center.
            const float FillRight = ImClamp(KnobX, TrackMin.x + Rounding, TrackMax.x);
            if (FillRight > TrackMin.x)
            {
                DL->AddRectFilled(TrackMin, ImVec2(FillRight, TrackMax.y), FillCol, Rounding, ImDrawFlags_RoundCornersLeft);
                if (EnumHasAnyFlags(Flags, ESliderFlags::FillGradient))
                {
                    // Overlay a left->knob gradient; color at the knob reflects its position along the full range.
                    const ImU32 EndCol = LerpColor(FillCol, FillEndCol, T);
                    DL->AddRectFilledMultiColor(ImVec2(TrackMin.x + Rounding, TrackMin.y), ImVec2(FillRight, TrackMax.y), FillCol, EndCol, EndCol, FillCol);
                }
            }

            // Glow halo behind the knob.
            if (EnumHasAnyFlags(Flags, ESliderFlags::Glow) && (Held || Hovered))
            {
                const ImU32 GlowInner = ImGui::ColorConvertFloat4ToU32(ImGui::ColorConvertU32ToFloat4(FillEndCol) * ImVec4(1, 1, 1, 0.30f));
                const ImU32 GlowOuter = ImGui::ColorConvertFloat4ToU32(ImGui::ColorConvertU32ToFloat4(FillEndCol) * ImVec4(1, 1, 1, 0.0f));
                DL->AddCircleFilled(KnobCenter, KnobRadius * 2.1f, GlowOuter);
                DL->AddCircleFilled(KnobCenter, KnobRadius * 1.6f, GlowInner);
            }

            // A drop shadow, body, colored rim and top highlight, so the knob reads as a sphere.
            DL->AddCircleFilled(KnobCenter + ImVec2(0.0f, KnobRadius * 0.18f), KnobRadius, IM_COL32(0, 0, 0, 55));
            DL->AddCircleFilled(KnobCenter, KnobRadius, KnobCol);
            DL->AddCircleFilled(KnobCenter + ImVec2(0.0f, KnobRadius * 0.22f), KnobRadius * 0.85f, IM_COL32(0, 0, 0, 22));
            DL->AddCircleFilled(KnobCenter - ImVec2(KnobRadius * 0.28f, KnobRadius * 0.30f), KnobRadius * 0.55f, IM_COL32(255, 255, 255, 60));
            DL->AddCircle(KnobCenter, KnobRadius, LerpColor(FillCol, IM_COL32(0, 0, 0, 255), 0.15f), 0, ImMax(1.0f, 1.5f * Scale));

            // Optional value readout, right-aligned over the track.
            if (EnumHasAnyFlags(Flags, ESliderFlags::AlwaysValue))
            {
                char Buf[64];
                const char* BufEnd = Buf + ImGui::DataTypeFormatString(Buf, IM_ARRAYSIZE(Buf), DataType, Value, Format);
                const ImVec2 TextSize = ImGui::CalcTextSize(Buf, BufEnd);
                const ImVec2 TextPos(FrameBB.Max.x - TextSize.x, CenterY - TextSize.y * 0.5f);
                DL->AddText(TextPos + ImVec2(1, 1), IM_COL32(0, 0, 0, 110), Buf, BufEnd);
                DL->AddText(TextPos, ImGui::GetColorU32(ImGuiCol_Text), Buf, BufEnd);
            }

            // Trailing label.
            if (LabelSize.x > 0.0f)
            {
                ImGui::RenderText(ImVec2(FrameBB.Max.x + Style.ItemInnerSpacing.x, FrameBB.Min.y + Style.FramePadding.y), Label);
            }

            if (EnumHasAnyFlags(Flags, ESliderFlags::ValueOnHover) && (Held || Hovered))
            {
                char Buf[64];
                ImGui::DataTypeFormatString(Buf, IM_ARRAYSIZE(Buf), DataType, Value, Format);
                ImGuiX::TextTooltip("{}", Buf);
            }

            return ValueChanged;
        }
    }

    bool SliderFloat(const char* Label, float* Value, float Min, float Max, ESliderFlags Flags, const char* Format, const FSliderStyle* Style)
    {
        return SliderScalarStyled(Label, ImGuiDataType_Float, Value, &Min, &Max, Flags, Format, Style);
    }

    bool SliderInt(const char* Label, int32* Value, int32 Min, int32 Max, ESliderFlags Flags, const char* Format, const FSliderStyle* Style)
    {
        return SliderScalarStyled(Label, ImGuiDataType_S32, Value, &Min, &Max, Flags, Format, Style);
    }

    bool SearchBar(const char* StrId, ImGuiTextFilter& Filter, const char* Hint)
    {
        const ImGuiStyle& Style = ImGui::GetStyle();
        const float ButtonWidth = ImGui::GetFrameHeight();

        // GetContentRegionAvail does not yet account for a scrollbar appearing on this same frame.
        const float Margin = Style.FramePadding.x;
        const float Avail  = ImGui::GetContentRegionAvail().x;

        ImGui::SetNextItemWidth(Math::Max(Avail - ButtonWidth - Style.ItemSpacing.x - Margin, 48.0f));
        bool bChanged = Filter.Draw(StrId);

        if (Hint != nullptr && !Filter.IsActive())
        {
            const ImVec2 Min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(Min.x + Style.FramePadding.x, Min.y + Style.FramePadding.y),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), Hint);
        }

        ImGui::SameLine();

        ImGui::PushID(StrId);
        ImGui::BeginDisabled(!Filter.IsActive());
        if (ImGui::Button(LE_ICON_CLOSE, ImVec2(ButtonWidth, 0.0f)))
        {
            Filter.Clear();
            bChanged = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();

        return bChanged;
    }

    bool PassSearchFilter(FStringView Query, FStringView Text)
    {
        const char* Word      = Query.data();
        const char* QueryEnd  = Query.data() + Query.size();
        const char* TextBegin = Text.data();
        const char* TextEnd   = Text.data() + Text.size();

        while (Word < QueryEnd)
        {
            while (Word < QueryEnd && *Word == ' ')
            {
                ++Word;
            }

            const char* WordEnd = Word;
            while (WordEnd < QueryEnd && *WordEnd != ' ')
            {
                ++WordEnd;
            }

            if (WordEnd == Word)
            {
                break;
            }

            if (TextBegin == TextEnd || ImStristr(TextBegin, TextEnd, Word, WordEnd) == nullptr)
            {
                return false;
            }

            Word = WordEnd;
        }

        // An empty box, or one holding only spaces, filters nothing out.
        return true;
    }

    bool PassSearchFilter(const ImGuiTextFilter& Filter, FStringView Text)
    {
        return PassSearchFilter(FStringView(Filter.InputBuf), Text);
    }

    int32 SearchableCombo(const char* StrId, const char* Preview, int32 ItemCount, int32 CurrentIndex, const TFunction<FFixedString(int32)>& GetItemLabel, const char* ItemIcon, FFixedString* OutCreatedText)
    {
        int32 Result = INDEX_NONE;
        if (OutCreatedText != nullptr)
        {
            OutCreatedText->clear();
        }
        const ImGuiStyle& Style = ImGui::GetStyle();

        const ImGuiID ComboId = ImGui::GetID(StrId);

        // Mirror the chosen item's icon into the closed preview so it matches the open list.
        FFixedString PreviewStr = Preview;
        if (ItemIcon != nullptr && CurrentIndex != INDEX_NONE)
        {
            PreviewStr = ItemIcon;
            PreviewStr += "  ";
            PreviewStr += Preview;
        }

        // Measured only while open, and ImGui honors a size constraint rather than forcing the button width.
        const ImGuiID PopupId = ImHashStr("##ComboPopup", 0, ComboId);
        if (ImGui::IsPopupOpen(PopupId, ImGuiPopupFlags_None))
        {
            float ContentWidth = 0.0f;
            for (int32 i = 0; i < ItemCount; ++i)
            {
                ContentWidth = ImMax(ContentWidth, ImGui::CalcTextSize(GetItemLabel(i).c_str()).x);
            }
            if (ItemIcon != nullptr)
            {
                ContentWidth += ImGui::CalcTextSize(ItemIcon).x + Style.ItemInnerSpacing.x * 2.0f;
            }

            const float Decorations = Style.FramePadding.x * 2.0f + Style.WindowPadding.x * 2.0f + Style.ScrollbarSize;
            const float MaxWidth    = ImGui::GetMainViewport()->WorkSize.x * 0.6f;
            const float FitWidth    = ImClamp(ContentWidth + Decorations, ImGui::CalcItemWidth(), MaxWidth);
            ImGui::SetNextWindowSizeConstraints(ImVec2(FitWidth, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        }

        if (ImGui::BeginCombo(StrId, PreviewStr.c_str(), ImGuiComboFlags_HeightLargest))
        {
            const float PopupWidth = ImGui::GetContentRegionAvail().x;

            static THashMap<ImGuiID, ImGuiTextFilter> Filters;
            ImGuiTextFilter& Filter = Filters[ComboId];

            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            Filter.Draw("##filter", PopupWidth);

            // Gray magnify hint while the search box is empty.
            if (!Filter.IsActive())
            {
                ImVec2 HintPos = ImGui::GetItemRectMin();
                HintPos.x += Style.FramePadding.x + 2.0f;
                HintPos.y += Style.FramePadding.y;
                ImGui::GetWindowDrawList()->AddText(HintPos, ImGui::GetColorU32(ImGuiCol_TextDisabled), LE_ICON_MAGNIFY " Search...");
            }

            ImGui::Separator();

            // Pushed first so the height math measures the rows actually drawn rather than default spacing.
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Style.ItemSpacing.x, 2.0f));

            // Reserves at least one row, so the empty-list message is never clipped.
            const int32 VisibleRows = ImClamp(ItemCount, 1, 12);
            const float ListHeight = VisibleRows * ImGui::GetTextLineHeightWithSpacing() + Style.FramePadding.y * 2.0f;

            if (ImGui::BeginChild("##list", ImVec2(PopupWidth, ListHeight)))
            {
                bool bAnyVisible = false;

                if (OutCreatedText != nullptr && Filter.InputBuf[0] != 0)
                {
                    bool bExists = false;
                    for (int32 i = 0; i < ItemCount && !bExists; ++i)
                    {
                        bExists = strcmp(GetItemLabel(i).c_str(), Filter.InputBuf) == 0;
                    }

                    if (!bExists)
                    {
                        FFixedString Row = LE_ICON_PLUS "  Create \"";
                        Row += Filter.InputBuf;
                        Row += "\"";

                        if (ImGui::Selectable(Row.c_str()))
                        {
                            *OutCreatedText = Filter.InputBuf;
                            Filter.Clear();
                            ImGui::CloseCurrentPopup();
                        }
                        bAnyVisible = true;
                    }
                }

                for (int32 i = 0; i < ItemCount; ++i)
                {
                    const FFixedString Label = GetItemLabel(i);
                    if (!PassSearchFilter(Filter, Label.c_str()))
                    {
                        continue;
                    }
                    bAnyVisible = true;

                    FFixedString Row;
                    if (ItemIcon != nullptr)
                    {
                        Row = ItemIcon;
                        Row += "  ";
                        Row += Label;
                    }
                    else
                    {
                        Row = Label;
                    }

                    ImGui::PushID(i);
                    const bool bSelected = (i == CurrentIndex);
                    if (ImGui::Selectable(Row.c_str(), bSelected))
                    {
                        Result = i;
                        ImGui::CloseCurrentPopup();
                    }
                    if (bSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                    ImGui::PopID();
                }

                if (!bAnyVisible)
                {
                    ImGui::TextDisabled("No matches");
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();

            ImGui::EndCombo();
        }

        return Result;
    }

    bool AssetReferenceCombo(const char* StrId, CClass* FilterClass, FGuid& InOutGUID, const char* ItemIcon)
    {
        if (FilterClass == nullptr)
        {
            return false;
        }

        // Every asset whose class is-a FilterClass, sorted by name.
        TVector<FAssetData*> Assets = FAssetRegistry::Get().FindByPredicate([FilterClass](const FAssetData& Data)
        {
            CClass* DataClass = FindObject<CClass>(Data.AssetClass);
            return DataClass != nullptr && DataClass->IsChildOf(FilterClass);
        });

        Algo::Sort(Assets, [](const FAssetData* A, const FAssetData* B)
        {
            return A->AssetName.ToString() < B->AssetName.ToString();
        });

        int32 CurrentIndex = INDEX_NONE;
        for (int32 i = 0; i < (int32)Assets.size(); ++i)
        {
            if (Assets[i]->AssetGUID == InOutGUID)
            {
                CurrentIndex = i;
                break;
            }
        }

        const char* Preview = (CurrentIndex != INDEX_NONE) ? Assets[CurrentIndex]->AssetName.c_str() : "Select an asset...";

        const int32 Picked = SearchableCombo(StrId, Preview, (int32)Assets.size(), CurrentIndex,
            [&Assets](int32 Index) { return FFixedString(Assets[Index]->AssetName.c_str()); }, ItemIcon);

        if (Picked != INDEX_NONE && Picked != CurrentIndex)
        {
            InOutGUID = Assets[Picked]->AssetGUID;
            return true;
        }
        return false;
    }

    bool ClassCombo(const char* StrId, CClass* BaseClass, CClass*& InOutClass, bool bAllowNone, const char* ItemIcon)
    {
        TVector<CClass*> Candidates;
        for (TObjectIterator<CClass> It; It; ++It)
        {
            CClass* Candidate = *It;
            if (BaseClass == nullptr || Candidate->IsChildOf(BaseClass))
            {
                Candidates.push_back(Candidate);
            }
        }

        Algo::Sort(Candidates, [](CClass* A, CClass* B)
        {
            return strcmp(A->GetName().c_str(), B->GetName().c_str()) < 0;
        });

        // "None" occupies index 0 when allowed, so every candidate sits one slot further along.
        const int32 Offset = bAllowNone ? 1 : 0;

        int32 CurrentIndex = bAllowNone ? 0 : INDEX_NONE;
        for (int32 i = 0; i < (int32)Candidates.size(); ++i)
        {
            if (Candidates[i] == InOutClass)
            {
                CurrentIndex = i + Offset;
                break;
            }
        }

        // Copied, since c_str hands back a rotating buffer the per-item labels below would overwrite.
        const FFixedString Preview = InOutClass ? FFixedString(InOutClass->GetName().c_str())
                                                : FFixedString(bAllowNone ? "None" : "Select a class...");

        const int32 Picked = SearchableCombo(StrId, Preview.c_str(), (int32)Candidates.size() + Offset, CurrentIndex,
            [&Candidates, Offset](int32 Index) -> FFixedString
            {
                return (Index < Offset) ? FFixedString("None") : FFixedString(Candidates[Index - Offset]->GetName().c_str());
            }, ItemIcon);

        if (Picked != INDEX_NONE && Picked != CurrentIndex)
        {
            InOutClass = (Picked < Offset) ? nullptr : Candidates[Picked - Offset];
            return true;
        }
        return false;
    }

    bool StructCombo(const char* StrId, CStruct* BaseStruct, CStruct*& InOutStruct, bool bAllowNone, const char* ItemIcon)
    {
        TVector<CStruct*> Candidates;
        for (TObjectIterator<CStruct> It; It; ++It)
        {
            CStruct* Candidate = *It;
            if (Candidate == BaseStruct || !IsInstancableStructType(Candidate))
            {
                continue;
            }
            if (BaseStruct == nullptr || Candidate->IsChildOf(BaseStruct))
            {
                Candidates.push_back(Candidate);
            }
        }

        Algo::Sort(Candidates, [](CStruct* A, CStruct* B)
        {
            return strcmp(A->GetName().c_str(), B->GetName().c_str()) < 0;
        });

        const int32 Offset = bAllowNone ? 1 : 0;

        int32 CurrentIndex = bAllowNone ? 0 : INDEX_NONE;
        for (int32 i = 0; i < (int32)Candidates.size(); ++i)
        {
            if (Candidates[i] == InOutStruct)
            {
                CurrentIndex = i + Offset;
                break;
            }
        }

        // Copied for the same reason ClassCombo copies, since c_str reuses a thread-local buffer.
        const FFixedString Preview = InOutStruct ? FFixedString(InOutStruct->GetName().c_str())
                                                 : FFixedString(bAllowNone ? "None" : "Select a type...");

        const int32 Picked = SearchableCombo(StrId, Preview.c_str(), (int32)Candidates.size() + Offset, CurrentIndex,
            [&Candidates, Offset](int32 Index) -> FFixedString
            {
                return (Index < Offset) ? FFixedString("None") : FFixedString(Candidates[Index - Offset]->GetName().c_str());
            }, ItemIcon);

        if (Picked != INDEX_NONE && Picked != CurrentIndex)
        {
            InOutStruct = (Picked < Offset) ? nullptr : Candidates[Picked - Offset];
            return true;
        }
        return false;
    }

    ImTextureRef ToImTextureRef(const RHI::FManagedTexture& Texture)
    {
        return ToImTextureRef(Texture.IsValid() ? Texture.SampledSlot : ~0u);
    }

    ImTextureRef ToImTextureRef(FStringView Path)
    {
		#if WITH_EDITOR
        return Render().GetImGuiRenderer()->GetOrCreateImTexture(Path);
		#else
    	return {};
		#endif
    }

    ImTextureRef ToImTextureRef(uint32 ResourceID)
    {
        if (ResourceID == ~0u)
        {
            ResourceID = RHI::Textures::DefaultResourceID();
        }
        return ImTextureRef((ImTextureID)ResourceID);
    }

    FString FormatSize(size_t Bytes)
    {
        const char* Suffixes[] = { "B", "KB", "MB", "GB" };
        double Size = static_cast<double>(Bytes);
        int Suffix = 0;

        while (Size >= 1024.0 && Suffix < 3)
        {
            Size /= 1024.0;
            ++Suffix;
        }
    	FString Value;
    	AppendFormat(Value, "{:.2f} {}", Size, Suffixes[Suffix]);
        return Value;
    }

    void RenderWindowOuterBorders(ImGuiWindow* Window)
    {
    	struct ImGuiResizeBorderDef
		{
			ImVec2 InnerDir;
			ImVec2 SegmentN1, SegmentN2;
			float  OuterAngle;
		};

		static const ImGuiResizeBorderDef resize_border_def[4] =
		{
			{ ImVec2(+1, 0), ImVec2(0, 1), ImVec2(0, 0), IM_PI * 1.00f }, // Left
			{ ImVec2(-1, 0), ImVec2(1, 0), ImVec2(1, 1), IM_PI * 0.00f }, // Right
			{ ImVec2(0, +1), ImVec2(0, 0), ImVec2(1, 0), IM_PI * 1.50f }, // Up
			{ ImVec2(0, -1), ImVec2(1, 1), ImVec2(0, 1), IM_PI * 0.50f }  // Down
		};

		auto GetResizeBorderRect = [](ImGuiWindow* window, int border_n, float perp_padding, float thickness)
		{
			ImRect rect = window->Rect();
			if (thickness == 0.0f)
			{
				rect.Max.x -= 1;
				rect.Max.y -= 1;
			}
			if (border_n == ImGuiDir_Left) { return ImRect(rect.Min.x - thickness, rect.Min.y + perp_padding, rect.Min.x + thickness, rect.Max.y - perp_padding); }
			if (border_n == ImGuiDir_Right) { return ImRect(rect.Max.x - thickness, rect.Min.y + perp_padding, rect.Max.x + thickness, rect.Max.y - perp_padding); }
			if (border_n == ImGuiDir_Up) { return ImRect(rect.Min.x + perp_padding, rect.Min.y - thickness, rect.Max.x - perp_padding, rect.Min.y + thickness); }
			if (border_n == ImGuiDir_Down) { return ImRect(rect.Min.x + perp_padding, rect.Max.y - thickness, rect.Max.x - perp_padding, rect.Max.y + thickness); }
			IM_ASSERT(0);
			return ImRect();
		};


		ImGuiContext& g = *GImGui;
		float rounding = Window->WindowRounding;
		float border_size = 1.0f; // window->WindowBorderSize;
		if (border_size > 0.0f && !(Window->Flags & ImGuiWindowFlags_NoBackground))
		{
			Window->DrawList->AddRect(Window->Pos, { Window->Pos.x + Window->Size.x,  Window->Pos.y + Window->Size.y }, ImGui::GetColorU32(ImGuiCol_Border), rounding, 0, border_size);
		}

	    int border_held = Window->ResizeBorderHeld;
		if (border_held != -1)
		{
			const ImGuiResizeBorderDef& def = resize_border_def[border_held];
			ImRect border_r = GetResizeBorderRect(Window, border_held, rounding, 0.0f);
			ImVec2 p1 = ImLerp(border_r.Min, border_r.Max, def.SegmentN1);
			const float offsetX = def.InnerDir.x * rounding;
			const float offsetY = def.InnerDir.y * rounding;
			p1.x += 0.5f + offsetX;
			p1.y += 0.5f + offsetY;

			ImVec2 p2 = ImLerp(border_r.Min, border_r.Max, def.SegmentN2);
			p2.x += 0.5f + offsetX;
			p2.y += 0.5f + offsetY;

			Window->DrawList->PathArcTo(p1, rounding, def.OuterAngle - IM_PI * 0.25f, def.OuterAngle);
			Window->DrawList->PathArcTo(p2, rounding, def.OuterAngle, def.OuterAngle + IM_PI * 0.25f);
			Window->DrawList->PathStroke(ImGui::GetColorU32(ImGuiCol_SeparatorActive), 0, ImMax(2.0f, border_size)); // Thicker than usual
		}
		if (g.Style.FrameBorderSize > 0 && !(Window->Flags & ImGuiWindowFlags_NoTitleBar) && !Window->DockIsActive)
		{
			float y = Window->Pos.y + Window->TitleBarHeight - 1;
			Window->DrawList->AddLine(ImVec2(Window->Pos.x + border_size, y), ImVec2(Window->Pos.x + Window->Size.x - border_size, y), ImGui::GetColorU32(ImGuiCol_Border), g.Style.FrameBorderSize);
		}
    }

    bool UpdateWindowManualResize(ImGuiWindow* Window, ImVec2& NewSize, ImVec2& NewPosition)
    {
    	auto CalcWindowSizeAfterConstraint = [](ImGuiWindow* window, const ImVec2& size_desired)
		{
			ImGuiContext& g = *GImGui;
			ImVec2 new_size = size_desired;
			if (g.NextWindowData.WindowFlags & ImGuiNextWindowDataFlags_HasSizeConstraint)
			{
				// Using -1,-1 on either X/Y axis to preserve the current size.
				ImRect cr = g.NextWindowData.SizeConstraintRect;
				new_size.x = (cr.Min.x >= 0 && cr.Max.x >= 0) ? ImClamp(new_size.x, cr.Min.x, cr.Max.x) : window->SizeFull.x;
				new_size.y = (cr.Min.y >= 0 && cr.Max.y >= 0) ? ImClamp(new_size.y, cr.Min.y, cr.Max.y) : window->SizeFull.y;
				if (g.NextWindowData.SizeCallback)
				{
					ImGuiSizeCallbackData data;
					data.UserData = g.NextWindowData.SizeCallbackUserData;
					data.Pos = window->Pos;
					data.CurrentSize = window->SizeFull;
					data.DesiredSize = new_size;
					g.NextWindowData.SizeCallback(&data);
					new_size = data.DesiredSize;
				}
				new_size.x = Math::Floor(new_size.x);
				new_size.y = Math::Floor(new_size.y);
			}

			// Minimum size
			if (!(window->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_AlwaysAutoResize)))
			{
				ImGuiWindow* window_for_height = (window->DockNodeAsHost && window->DockNodeAsHost->VisibleWindow) ? window->DockNodeAsHost->VisibleWindow : window;
				const float decoration_up_height = window_for_height->TitleBarHeight + window_for_height->MenuBarHeight;
				new_size = ImMax(new_size, g.Style.WindowMinSize);
				new_size.y = ImMax(new_size.y, decoration_up_height + ImMax(0.0f, g.Style.WindowRounding - 1.0f)); // Reduce artifacts with very small windows
			}
			return new_size;
		};

		auto CalcWindowAutoFitSize = [CalcWindowSizeAfterConstraint](ImGuiWindow* window, const ImVec2& size_contents)
		{
			ImGuiContext& g = *GImGui;
			ImGuiStyle& style = g.Style;
			const float decoration_up_height = window->TitleBarHeight + window->MenuBarHeight;
			ImVec2 size_pad{ window->WindowPadding.x * 2.0f, window->WindowPadding.y * 2.0f };
			ImVec2 size_desired = { size_contents.x + size_pad.x + 0.0f, size_contents.y + size_pad.y + decoration_up_height };
			if (window->Flags & ImGuiWindowFlags_Tooltip)
			{
				// Tooltip always resize
				return size_desired;
			}
			else
			{
				// Maximum window size is determined by the viewport size or monitor size
				const bool is_popup = (window->Flags & ImGuiWindowFlags_Popup) != 0;
				const bool is_menu = (window->Flags & ImGuiWindowFlags_ChildMenu) != 0;
				ImVec2 size_min = style.WindowMinSize;
				if (is_popup || is_menu) // Popups and menus bypass style.WindowMinSize by default, but we give then a non-zero minimum size to facilitate understanding problematic cases (e.g. empty popups)
					size_min = ImMin(size_min, ImVec2(4.0f, 4.0f));

				// FIXME may want the work size rather than the full size, depending on the window type.
				ImVec2 avail_size = window->Viewport->Size;
				if (window->ViewportOwned)
				{
					avail_size = ImVec2(FLT_MAX, FLT_MAX);
				}
				const int monitor_idx = window->ViewportAllowPlatformMonitorExtend;
				if (monitor_idx >= 0 && monitor_idx < g.PlatformIO.Monitors.Size)
				{
					avail_size = g.PlatformIO.Monitors[monitor_idx].WorkSize;
				}
				ImVec2 size_auto_fit = ImClamp(size_desired, size_min, ImMax(size_min, { avail_size.x - style.DisplaySafeAreaPadding.x * 2.0f,
					                                                             avail_size.y - style.DisplaySafeAreaPadding.y * 2.0f }));

				// Grows the other axis to compensate for an expected scrollbar. FIXME may exceed the viewport.
				ImVec2 size_auto_fit_after_constraint = CalcWindowSizeAfterConstraint(window, size_auto_fit);
				bool will_have_scrollbar_x = (size_auto_fit_after_constraint.x - size_pad.x - 0.0f < size_contents.x && !(window->Flags & ImGuiWindowFlags_NoScrollbar) && (window->Flags & ImGuiWindowFlags_HorizontalScrollbar)) || (window->Flags & ImGuiWindowFlags_AlwaysHorizontalScrollbar);
				bool will_have_scrollbar_y = (size_auto_fit_after_constraint.y - size_pad.y - decoration_up_height < size_contents.y && !(window->Flags& ImGuiWindowFlags_NoScrollbar)) || (window->Flags & ImGuiWindowFlags_AlwaysVerticalScrollbar);
				if (will_have_scrollbar_x)
				{
					size_auto_fit.y += style.ScrollbarSize;
				}
				if (will_have_scrollbar_y)
				{
					size_auto_fit.x += style.ScrollbarSize;
				}
				return size_auto_fit;
			}
		};

		ImGuiContext& g = *GImGui;

		// Decide if we are going to handle borders and resize grips
		const bool handle_borders_and_resize_grips = (Window->DockNodeAsHost || !Window->DockIsActive);

		if (!handle_borders_and_resize_grips || Window->Collapsed)
		{
			return false;
		}

	    const ImVec2 size_auto_fit = CalcWindowAutoFitSize(Window, Window->ContentSizeIdeal);

		// Handles manual resize through grips, borders and gamepad.
		int border_held = -1;
		[[maybe_unused]] ImU32 resize_grip_col[4] = {};
		const int resize_grip_count = g.IO.ConfigWindowsResizeFromEdges ? 2 : 1; // Allow resize from lower-left if we have the mouse cursor feedback for it.
		Window->ResizeBorderHeld = (signed char)border_held;

		//const ImRect& visibility_rect;

		struct ImGuiResizeBorderDef
		{
			ImVec2 InnerDir;
			ImVec2 SegmentN1, SegmentN2;
			float  OuterAngle;
		};
		static const ImGuiResizeBorderDef resize_border_def[4] =
		{
			{ ImVec2(+1, 0), ImVec2(0, 1), ImVec2(0, 0), IM_PI * 1.00f }, // Left
			{ ImVec2(-1, 0), ImVec2(1, 0), ImVec2(1, 1), IM_PI * 0.00f }, // Right
			{ ImVec2(0, +1), ImVec2(0, 0), ImVec2(1, 0), IM_PI * 1.50f }, // Up
			{ ImVec2(0, -1), ImVec2(1, 1), ImVec2(0, 1), IM_PI * 0.50f }  // Down
		};

		// Data for resizing from corner
		struct ImGuiResizeGripDef
		{
			ImVec2  CornerPosN;
			ImVec2  InnerDir;
			int     AngleMin12, AngleMax12;
		};
		static const ImGuiResizeGripDef resize_grip_def[4] =
		{
			{ ImVec2(1, 1), ImVec2(-1, -1), 0, 3 },  // Lower-right
			{ ImVec2(0, 1), ImVec2(+1, -1), 3, 6 },  // Lower-left
			{ ImVec2(0, 0), ImVec2(+1, +1), 6, 9 },  // Upper-left (Unused)
			{ ImVec2(1, 0), ImVec2(-1, +1), 9, 12 }  // Upper-right (Unused)
		};

		auto CalcResizePosSizeFromAnyCorner = [CalcWindowSizeAfterConstraint](ImGuiWindow* window, const ImVec2& corner_target, const ImVec2& corner_norm, ImVec2* out_pos, ImVec2* out_size)
		{
			ImVec2 pos_min = ImLerp(corner_target, window->Pos, corner_norm);                // Expected window upper-left
			ImVec2 pos_max = ImLerp({ window->Pos.x + window->Size.x, window->Pos.y + window->Size.y }, corner_target, corner_norm); // Expected window lower-right
			ImVec2 size_expected = { pos_max.x - pos_min.x,  pos_max.y - pos_min.y };
			ImVec2 size_constrained = CalcWindowSizeAfterConstraint(window, size_expected);
			*out_pos = pos_min;
			if (corner_norm.x == 0.0f)
			{
				out_pos->x -= (size_constrained.x - size_expected.x);
			}
			if (corner_norm.y == 0.0f)
			{
				out_pos->y -= (size_constrained.y - size_expected.y);
			}
			*out_size = size_constrained;
		};

		auto GetResizeBorderRect = [](ImGuiWindow* window, int border_n, float perp_padding, float thickness)
		{
			ImRect rect = window->Rect();
			if (thickness == 0.0f)
			{
				rect.Max.x -= 1;
				rect.Max.y -= 1;
			}
			if (border_n == ImGuiDir_Left) { return ImRect(rect.Min.x - thickness, rect.Min.y + perp_padding, rect.Min.x + thickness, rect.Max.y - perp_padding); }
			if (border_n == ImGuiDir_Right) { return ImRect(rect.Max.x - thickness, rect.Min.y + perp_padding, rect.Max.x + thickness, rect.Max.y - perp_padding); }
			if (border_n == ImGuiDir_Up) { return ImRect(rect.Min.x + perp_padding, rect.Min.y - thickness, rect.Max.x - perp_padding, rect.Min.y + thickness); }
			if (border_n == ImGuiDir_Down) { return ImRect(rect.Min.x + perp_padding, rect.Max.y - thickness, rect.Max.x - perp_padding, rect.Max.y + thickness); }
			IM_ASSERT(0);
			return ImRect();
		};

		static const float WINDOWS_HOVER_PADDING = 4.0f;                        // Extend outside window for hovering/resizing (maxxed with TouchPadding) and inside windows for borders. Affect FindHoveredWindow().
		static const float WINDOWS_RESIZE_FROM_EDGES_FEEDBACK_TIMER = 0.04f;    // Reduce visual noise by only highlighting the border after a certain time.

		auto& style = g.Style;
		ImGuiWindowFlags flags = Window->Flags;

		if (/*(flags & ImGuiWindowFlags_NoResize) || */(flags & ImGuiWindowFlags_AlwaysAutoResize) || Window->AutoFitFramesX > 0 || Window->AutoFitFramesY > 0)
		{
			return false;
		}
    	if (Window->WasActive == false) // Early out to avoid running this code for e.g. an hidden implicit/fallback Debug window.
    	{
    		return false;
    	}

    	[[maybe_unused]] bool ret_auto_fit = false;
		const int resize_border_count = g.IO.ConfigWindowsResizeFromEdges ? 4 : 0;
		const float grip_draw_size = Math::Floor(ImMax(g.FontSize * 1.35f, Window->WindowRounding + 1.0f + g.FontSize * 0.2f));
		const float grip_hover_inner_size = Math::Floor(grip_draw_size * 0.75f);
		const float grip_hover_outer_size = g.IO.ConfigWindowsResizeFromEdges ? WINDOWS_HOVER_PADDING : 0.0f;

		ImVec2 pos_target(FLT_MAX, FLT_MAX);
		ImVec2 size_target(FLT_MAX, FLT_MAX);

		// Clamping to stay visible enforces that the window position stays inside the visibility rect.
		ImRect viewport_rect(Window->Viewport->GetMainRect());
		ImRect viewport_work_rect(Window->Viewport->GetWorkRect());
		ImVec2 visibility_padding = ImMax(style.DisplayWindowPadding, style.DisplaySafeAreaPadding);
		ImRect visibility_rect({ viewport_work_rect.Min.x + visibility_padding.x, viewport_work_rect.Min.y + visibility_padding.y },
			{ viewport_work_rect.Max.x - visibility_padding.x, viewport_work_rect.Max.y - visibility_padding.y });

		// Only interaction is clipped, so the clip rect is overwritten rather than pushed before setup.
		const bool clip_with_viewport_rect = !(g.IO.BackendFlags & ImGuiBackendFlags_HasMouseHoveredViewport) || (g.IO.MouseHoveredViewport != Window->ViewportId) || !(Window->Viewport->Flags & ImGuiViewportFlags_NoDecoration);
		if (clip_with_viewport_rect)
		{
			Window->ClipRect = Window->Viewport->GetMainRect();
		}

    	// Resize grips and borders are on layer 1
		Window->DC.NavLayerCurrent = ImGuiNavLayer_Menu;

		// Manual resize grips
		ImGui::PushID("#RESIZE");
		for (int resize_grip_n = 0; resize_grip_n < resize_grip_count; resize_grip_n++)
		{
			const ImGuiResizeGripDef& def = resize_grip_def[resize_grip_n];

			const ImVec2 corner = ImLerp(Window->Pos, { Window->Pos.x + Window->Size.x, Window->Pos.y + Window->Size.y }, def.CornerPosN);

			// Using the FlattenChilds button flag we make the resize button accessible even if we are hovering over a child window
			bool hovered, held;
			const ImVec2 min = { corner.x - def.InnerDir.x * grip_hover_outer_size, corner.y - def.InnerDir.y * grip_hover_outer_size };
			const ImVec2 max = { corner.x + def.InnerDir.x * grip_hover_outer_size, corner.y + def.InnerDir.y * grip_hover_outer_size };
			ImRect resize_rect(min, max);

			if (resize_rect.Min.x > resize_rect.Max.x)
			{
				ImSwap(resize_rect.Min.x, resize_rect.Max.x);
			}
			if (resize_rect.Min.y > resize_rect.Max.y)
			{
				ImSwap(resize_rect.Min.y, resize_rect.Max.y);
			}
			ImGuiID resize_grip_id = Window->GetID(resize_grip_n); // == GetWindowResizeCornerID()
			ImGui::ButtonBehavior(resize_rect, resize_grip_id, &hovered, &held, ImGuiButtonFlags_FlattenChildren | ImGuiButtonFlags_NoNavFocus);
			//GetForegroundDrawList(window)->AddRect(resize_rect.Min, resize_rect.Max, IM_COL32(255, 255, 0, 255));
			if (hovered || held)
				g.MouseCursor = (resize_grip_n & 1) ? ImGuiMouseCursor_ResizeNESW : ImGuiMouseCursor_ResizeNWSE;

			if (held && g.IO.MouseDoubleClicked[0] && resize_grip_n == 0)
			{
				// Manual auto-fit when double-clicking
				size_target = CalcWindowSizeAfterConstraint(Window, size_auto_fit);
				ret_auto_fit = true;
				ImGui::ClearActiveID();
			}
			else if (held)
			{
				// An absolute target size from the mouse position, rather than an incremental delta.
				ImVec2 clamp_min = ImVec2(def.CornerPosN.x == 1.0f ? visibility_rect.Min.x : -FLT_MAX, def.CornerPosN.y == 1.0f ? visibility_rect.Min.y : -FLT_MAX);
				ImVec2 clamp_max = ImVec2(def.CornerPosN.x == 0.0f ? visibility_rect.Max.x : +FLT_MAX, def.CornerPosN.y == 0.0f ? visibility_rect.Max.y : +FLT_MAX);

				const float x = g.IO.MousePos.x - g.ActiveIdClickOffset.x + ImLerp(def.InnerDir.x * grip_hover_outer_size, def.InnerDir.x * -grip_hover_inner_size, def.CornerPosN.x);
				const float y = g.IO.MousePos.y - g.ActiveIdClickOffset.y + ImLerp(def.InnerDir.y * grip_hover_outer_size, def.InnerDir.y * -grip_hover_inner_size, def.CornerPosN.y);

				ImVec2 corner_target(x, y); // Corner of the window corresponding to our corner grip
				corner_target = ImClamp(corner_target, clamp_min, clamp_max);
				CalcResizePosSizeFromAnyCorner(Window, corner_target, def.CornerPosN, &pos_target, &size_target);
			}

			// Only lower-left grip is visible before hovering/activating
			if (resize_grip_n == 0 || held || hovered)
			{
				resize_grip_col[resize_grip_n] = ImGui::GetColorU32(held ? ImGuiCol_ResizeGripActive : hovered ? ImGuiCol_ResizeGripHovered : ImGuiCol_ResizeGrip);
			}
		}
		for (int border_n = 0; border_n < resize_border_count; border_n++)
		{
			const ImGuiResizeBorderDef& def = resize_border_def[border_n];
			const ImGuiAxis axis = (border_n == ImGuiDir_Left || border_n == ImGuiDir_Right) ? ImGuiAxis_X : ImGuiAxis_Y;

			bool hovered, held;
			ImRect border_rect = GetResizeBorderRect(Window, border_n, grip_hover_inner_size, WINDOWS_HOVER_PADDING);
			ImGuiID border_id = Window->GetID(border_n + 4); // == GetWindowResizeBorderID()
			ImGui::ButtonBehavior(border_rect, border_id, &hovered, &held, ImGuiButtonFlags_FlattenChildren);
			//GetForegroundDrawLists(window)->AddRect(border_rect.Min, border_rect.Max, IM_COL32(255, 255, 0, 255));
			if ((hovered && g.HoveredIdTimer > WINDOWS_RESIZE_FROM_EDGES_FEEDBACK_TIMER) || held)
			{
				g.MouseCursor = (axis == ImGuiAxis_X) ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS;
				if (held)
				{
					border_held = border_n;
				}
			}
			if (held)
			{
				ImVec2 clamp_min(border_n == ImGuiDir_Right ? visibility_rect.Min.x : -FLT_MAX, border_n == ImGuiDir_Down ? visibility_rect.Min.y : -FLT_MAX);
				ImVec2 clamp_max(border_n == ImGuiDir_Left ? visibility_rect.Max.x : +FLT_MAX, border_n == ImGuiDir_Up ? visibility_rect.Max.y : +FLT_MAX);
				ImVec2 border_target = Window->Pos;
				border_target[axis] = g.IO.MousePos[axis] - g.ActiveIdClickOffset[axis] + WINDOWS_HOVER_PADDING;
				border_target = ImClamp(border_target, clamp_min, clamp_max);
				CalcResizePosSizeFromAnyCorner(Window, border_target, ImMin(def.SegmentN1, def.SegmentN2), &pos_target, &size_target);
			}
		}
		ImGui::PopID();

		bool changed = false;
		NewSize = Window->Size;
		NewPosition = Window->Pos;

		// Apply back modified position/size to window
		if (size_target.x != FLT_MAX)
		{
			Window->SizeFull = size_target;
			ImGui::MarkIniSettingsDirty(Window);
			NewSize = size_target;
			changed = true;
		}
		if (pos_target.x != FLT_MAX)
		{
			Window->Pos = ImFloor(pos_target);
			ImGui::MarkIniSettingsDirty(Window);
			NewPosition = pos_target;
			changed = true;
		}

		Window->Size = Window->SizeFull;
		return changed;
    }


    namespace
    {
        // Transparent is the point, since painting the child background drew the seams between sections.
        bool BeginTitleBarSection(const char* ID, const ImVec2& Position, const ImVec2& Size, bool bMenuBar)
        {
            ImGui::SetCursorPos(Position);

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

            // Taller content then overhangs the text row, and menu popups open flush against the bar.
            const ImGuiStyle& Style = ImGui::GetStyle();
            if (bMenuBar)
            {
                const float PaddingY = Math::Max(Style.FramePadding.y, (Size.y - ImGui::GetFontSize()) * 0.5f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Style.FramePadding.x, PaddingY));
            }

            const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | (bMenuBar ? ImGuiWindowFlags_MenuBar : 0);
            const bool bVisible = ImGui::BeginChild(ID, Size, ImGuiChildFlags_None, Flags);

            if (bMenuBar)
            {
                ImGui::PopStyleVar();
            }
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();

            return bVisible;
        }
    }

    float FApplicationTitleBar::GetHeight()
    {
        return UnscaledBarHeight * GetUIScale();
    }

    float FApplicationTitleBar::GetWindowControlsWidth()
    {
        return UnscaledButtonWidth * 3.0f * GetUIScale();
    }

    float FApplicationTitleBar::GetContentRowHeight()
    {
        return ImGui::GetTextLineHeight();
    }

    void FApplicationTitleBar::DrawWindowControls()
    {
        FWindow* Window = Windowing::GetPrimaryWindowHandle();
        if (Window == nullptr)
        {
            return;
        }

        // Their hit boxes reach the window edge, which is what the OS does and what Fitts' law wants.
        const ImVec2 ButtonSize(UnscaledButtonWidth * GetUIScale(), ImGui::GetContentRegionAvail().y);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

        // Drawn with the stock button, since the flat one scales a transparent background and shows none.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.16f));

        if (ImGui::Button(LE_ICON_WINDOW_MINIMIZE "##Minimize", ButtonSize))
        {
            Window->Minimize();
        }

        ImGui::SameLine();

        const bool bMaximized = Window->IsWindowMaximized();
        if (ImGui::Button(bMaximized ? LE_ICON_WINDOW_RESTORE "##Restore" : LE_ICON_WINDOW_MAXIMIZE "##Restore", ButtonSize))
        {
            if (bMaximized)
            {
                Window->Restore();
            }
            else
            {
                Window->Maximize();
            }
        }

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.13f, 0.13f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.10f, 0.10f, 1.0f));
        if (ImGui::Button(LE_ICON_WINDOW_CLOSE "##Close", ButtonSize))
        {
            FApplication::RequestExit();
        }
        ImGui::PopStyleColor(2);

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);
    }

    void FApplicationTitleBar::Draw(TFunction<void()>&& MenuSectionDrawFunction, TFunction<void()>&& InfoSectionDrawFunction, float InfoSectionWidth)
    {
        Rect = FVector4(0.0f);

        // Fonts/style scale with DPI, so the bar's fixed pixel sizes must too.
        const float Scale          = GetUIScale();
        const float BarHeight      = GetHeight();
        const float Padding        = UnscaledSectionPadding * Scale;
        const float ControlsWidth  = GetWindowControlsWidth();

        // Sections are positioned by hand and span the full height, so the bar carries no padding.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        const bool bBarVisible = ImGui::BeginViewportSideBar("##ApplicationTitleBar", ImGui::GetMainViewport(),
            ImGuiDir_Up, BarHeight, ImGuiWindowFlags_NoDecoration);
        ImGui::PopStyleVar(2);

        if (!bBarVisible)
        {
            ImGui::End();
            return;
        }

        const ImVec2 BarPos  = ImGui::GetWindowPos();
        const ImVec2 BarSize = ImGui::GetWindowSize();
        Rect = FVector4(BarPos.x, BarPos.y, BarSize.x, BarSize.y);

        // Nothing is sized from a constant, so a long project name is bounded by the window.
        const float ControlsX = Math::Max(0.0f, BarSize.x - ControlsWidth);

        const float MaxInfoWidth = Math::Max(0.0f, ControlsX - Padding * 2.0f);
        const float InfoWidth    = Math::Min(InfoSectionWidth, MaxInfoWidth);
        const float InfoX        = ControlsX - Padding - InfoWidth;

        const float MenuX     = Padding;
        const float MenuWidth = Math::Max(0.0f, InfoX - Padding - MenuX);

        // A child with a menu bar reports its origin below it, so an absolute Y would push menus off.
        const float RowOffsetY = ImFloor((BarHeight - GetContentRowHeight()) * 0.5f);

        if (MenuSectionDrawFunction && MenuWidth > 0.0f)
        {
            const bool bVisible = BeginTitleBarSection("##MenuSection", ImVec2(MenuX, 0.0f), ImVec2(MenuWidth, BarHeight), true);
            if (bVisible)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(16.0f * Scale, 8.0f * Scale));
                if (ImGui::BeginMenuBar())
                {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + RowOffsetY);
                    MenuSectionDrawFunction();
                    ImGui::EndMenuBar();
                }
                ImGui::PopStyleVar();
            }
            ImGui::EndChild();
        }

        if (InfoSectionDrawFunction && InfoWidth > 0.0f)
        {
            const bool bVisible = BeginTitleBarSection("##InfoSection", ImVec2(InfoX, 0.0f), ImVec2(InfoWidth, BarHeight), false);
            if (bVisible)
            {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + RowOffsetY);
                InfoSectionDrawFunction();
            }
            ImGui::EndChild();
        }

        if (BeginTitleBarSection("##WindowControls", ImVec2(ControlsX, 0.0f), ImVec2(ControlsWidth, BarHeight), false))
        {
            DrawWindowControls();
        }
        ImGui::EndChild();

        // Frees the sections from having to reserve a fixed drag gap between them.
        if (FWindow* MainWindow = Windowing::TryGetPrimaryWindowHandle())
        {
            const bool bOverBar    = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
            const bool bOverWidget = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive();
            MainWindow->SetTitleBarHovered(bOverBar && !bOverWidget);
        }

        ImGui::End();
    }
}
