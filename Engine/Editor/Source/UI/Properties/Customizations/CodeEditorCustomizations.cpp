#include "CodeEditorCustomizations.h"

#include "AssetPickerFilter.h"
#include "Containers/Algorithm.h"
#include "Session/SessionOps.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "UI/CodeEditor/CodeEditor.h"
#include "UI/CodeEditor/CodeEditorThemes.h"
#include "UI/Tools/EditorToolContext.h"

namespace Lumina
{
    namespace
    {
        constexpr double ThemeListSeconds = 2.0;

        EPropertyChangeOp DiscreteEdit(bool bCommitted, bool& bFinishPending)
        {
            if (bCommitted)
            {
                bFinishPending = true;
                return EPropertyChangeOp::Started;
            }
            if (bFinishPending)
            {
                bFinishPending = false;
                return EPropertyChangeOp::Finished;
            }
            return EPropertyChangeOp::None;
        }

        bool IsFileName(FStringView Name)
        {
            return !Name.empty() && Algo::NoneOf(Name.begin(), Name.end(), [](char C) { return C == '\0' || std::strchr("\\/:*?\"<>|", C) != nullptr; });
        }
    }

    EPropertyChangeOp FShortcutPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        const float Half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const bool bPrimaryWasListening = PrimaryField.IsCapturing();

        ImGui::PushID("Primary");
        bool bCommitted = PrimaryField.Draw(DisplayValue.Primary, Half, true);
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::PushID("Alternate");
        bCommitted |= AlternateField.Draw(DisplayValue.Alternate, Half, true);
        ImGui::PopID();

        // One field listens at a time, or a single key press would bind both.
        if (PrimaryField.IsCapturing() && AlternateField.IsCapturing())
        {
            bPrimaryWasListening ? PrimaryField.Cancel() : AlternateField.Cancel();
        }
        return DiscreteEdit(bCommitted, bFinishPending);
    }

    void FShortcutPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FShortcutPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        SShortcut ActualValue;
        Property->GetValue(&ActualValue);
        if (CachedValue.Primary != ActualValue.Primary || CachedValue.Alternate != ActualValue.Alternate)
        {
            CachedValue = DisplayValue = ActualValue;
            PrimaryField.Cancel();
            AlternateField.Cancel();
        }
    }

    EPropertyChangeOp FCodeEditorThemePropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        if (ImGui::GetTime() >= NextListTime)
        {
            NextListTime = ImGui::GetTime() + ThemeListSeconds;
            Themes = CodeEditorThemes::List();
        }

        int32 Current = Constants::kIndexNone;
        for (int32 Index = 0; Index < (int32)Themes.size(); ++Index)
        {
            if (Themes[Index] == DisplayValue.Name)
            {
                Current = Index;
            }
        }

        bool bCommitted = false;
        const float ButtonWidth = AssetPickerFilter::kButtonSize.x + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ButtonWidth);
        FFixedString Created;
        const int32 Picked = ImGuiX::SearchableCombo("##CodeTheme", DisplayValue.Name.c_str(), (int32)Themes.size(), Current,
            [this](int32 Index) { return FFixedString(Themes[Index].c_str()); }, LE_ICON_PALETTE, &Created);
        ImGuiX::TextTooltip("{}", "Type a name that is not listed to start a new theme from this one.");
        if (Picked != Constants::kIndexNone)
        {
            DisplayValue.Name = Themes[Picked];
            bCommitted = true;
        }
        else if (!Created.empty())
        {
            const FStringView NewName(Created.c_str(), Created.size());
            FCodeEditorPalette Palette = FCodeEditorPalette::Dark();
            CodeEditorThemes::Load(DisplayValue.Name.View(), Palette);
            if (!IsFileName(NewName))
            {
                ImGuiX::Notifications::NotifyError("A theme name cannot hold any of \\ / : * ? \" < > |");
            }
            else if (CodeEditorThemes::Save(NewName, Palette))
            {
                DisplayValue.Name.assign(NewName.data(), NewName.size());
                NextListTime = 0.0;
                bCommitted = true;
            }
            else
            {
                ImGuiX::Notifications::NotifyError("Could not write the theme file '{}'.", CodeEditorThemes::PathOf(NewName));
            }
        }

        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
        ImGui::BeginDisabled(Current == Constants::kIndexNone && !bCommitted);
        if (ImGui::Button(LE_ICON_PENCIL "##EditTheme", AssetPickerFilter::kButtonSize))
        {
            if (IEditorToolContext* Tools = SessionOps::GetToolContext())
            {
                Tools->OpenFileEditor(CodeEditorThemes::PathOf(DisplayValue.Name.View()).View());
            }
        }
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("{}", "Open the theme file. Code editors pick up each save.");

        return DiscreteEdit(bCommitted, bFinishPending);
    }

    void FCodeEditorThemePropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FCodeEditorThemePropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        SCodeEditorTheme ActualValue;
        Property->GetValue(&ActualValue);
        if (CachedValue.Name != ActualValue.Name)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }
}
