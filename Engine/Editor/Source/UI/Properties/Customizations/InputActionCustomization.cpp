#include "Containers/StringFormat.h"
#include <iterator>
#include "InputActionCustomization.h"

#include "imgui.h"
#include "Input/InputActionMap.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace InputActionPicker
    {
        const char* TypeLabel(EInputActionType Type)
        {
            switch (Type)
            {
            case EInputActionType::Axis1D: return "Axis";
            case EInputActionType::Axis2D: return "Axis 2D";
            default:                       return "Digital";
            }
        }

        const char* TypeIcon(EInputActionType Type)
        {
            switch (Type)
            {
            case EInputActionType::Axis1D: return LE_ICON_AXIS_ARROW;
            case EInputActionType::Axis2D: return LE_ICON_ARROW_ALL;
            default:                       return LE_ICON_GESTURE_TAP;
            }
        }

        FString DescribeBindings(const SInputAction& Action)
        {
            FString Result;
            for (const SInputActionBinding& Binding : Action.Bindings)
            {
                const FString Token = Binding.GetDisplayName();
                if (Token.IsEmpty())
                {
                    continue;
                }
                if (!Result.IsEmpty())
                {
                    Result += ", ";
                }
                Result += Token;
            }
            return Result.IsEmpty() ? FString("Unbound") : Result;
        }

        namespace
        {
            void DrawRightAlignedDim(const char* Text)
            {
                const float Width = ImGui::CalcTextSize(Text).x;
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - Width - ImGui::GetStyle().WindowPadding.x);
                ImGui::TextColored(EditorColors::TextMuted(), "%s", Text);
            }

            void DrawActionRowTooltip(const SInputAction& Action)
            {
                if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    return;
                }
                const FString Text = FString(TypeLabel(Action.Type)) + ":  " + DescribeBindings(Action);
                ImGuiX::TextTooltip_Internal(*Text);
            }
        }

        bool DrawCombo(const char* Id, FString& Value, ImGuiTextFilter& Filter)
        {
            const TVector<SInputAction>& Actions = FInputActionMap::Get().GetAllActions();

            const SInputAction* Current = nullptr;
            if (!Value.IsEmpty())
            {
                Current = FInputActionMap::Get().FindAction(FName(*Value));
            }

            // A name that resolves to nothing is the rename/delete case, and reads as bound until said so.
            const bool bDangling = !Value.IsEmpty() && Current == nullptr;

            FFixedString Preview;
            if (Value.IsEmpty())
            {
                Preview = LE_ICON_CLOSE_CIRCLE_OUTLINE "  None";
            }
            else if (bDangling)
            {
                AppendFormat(Preview, "{}  {}", LE_ICON_ALERT_CIRCLE_OUTLINE, *Value);
            }
            else
            {
                AppendFormat(Preview, "{}  {}", TypeIcon(Current->Type), *Value);
            }

            if (bDangling)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Warning());
            }

            const float Scale = ImGuiX::GetUIScale();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f * Scale, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
            const bool bOpen = ImGui::BeginCombo(Id, *Preview);

            if (bDangling)
            {
                ImGui::PopStyleColor();
                ImGuiX::TextTooltip("No action named '{}' in Settings > Engine > Input", *Value);
            }

            if (!bOpen)
            {
                return false;
            }

            if (ImGui::IsWindowAppearing())
            {
                Filter.Clear();
                ImGui::SetKeyboardFocusHere();
            }
            Filter.Draw("##ActionFilter", ImGui::GetContentRegionAvail().x);
            ImGui::Separator();

            bool bPicked = false;
            if (ImGui::Selectable(LE_ICON_CLOSE_CIRCLE_OUTLINE "  None", Value.IsEmpty()))
            {
                Value.Reset();
                bPicked = true;
            }

            int32 Shown = 0;
            for (const SInputAction& Action : Actions)
            {
                if (Action.Name.IsNone())
                {
                    continue;
                }
                const FString ActionName(*Action.Name);
                if (!ImGuiX::PassSearchFilter(Filter, *ActionName))
                {
                    continue;
                }

                ++Shown;

                FFixedString Row;
                AppendFormat(Row, "{}  {}", TypeIcon(Action.Type), *ActionName);
                if (ImGui::Selectable(*Row, Value == ActionName))
                {
                    Value = ActionName;
                    bPicked = true;
                }
                DrawActionRowTooltip(Action);
                DrawRightAlignedDim(TypeLabel(Action.Type));
            }

            if (Shown == 0)
            {
                ImGui::TextDisabled(Actions.IsEmpty()
                    ? "No input actions. Add them in Settings > Engine > Input."
                    : "No matching actions.");
            }

            ImGui::EndCombo();
            return bPicked;
        }
    }

    EPropertyChangeOp FInputActionHandlePropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        FString Picked = DisplayValue.Name.IsNone() ? FString() : FString(*DisplayValue.Name);

        if (InputActionPicker::DrawCombo("##inputactionhandle", Picked, SearchFilter))
        {
            DisplayValue.SetName(Picked.IsEmpty() ? FName() : FName(*Picked));
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

    void FInputActionHandlePropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FInputActionHandlePropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        SInputActionHandle ActualValue;
        Property->GetValue(&ActualValue);

        // Reset-to-default, undo/redo and multi-select must win, or the next write puts our stale name back.
        if (CachedValue.Name != ActualValue.Name)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }
}
