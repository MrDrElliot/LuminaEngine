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
                FString Token;
                switch (Binding.Source)
                {
                case EInputAxisSource::MouseX:     Token = "Mouse X"; break;
                case EInputAxisSource::MouseY:     Token = "Mouse Y"; break;
                case EInputAxisSource::MouseWheel: Token = "Mouse Wheel"; break;
                default:
                    if (Binding.Key.IsMouse())
                    {
                        Token = "Mouse " + FInputActionMap::MouseButtonToString(Binding.Key.MouseButton);
                    }
                    else if (Binding.Key.IsKeyboard())
                    {
                        if (Binding.Key.bCtrl)  { Token += "Ctrl+"; }
                        if (Binding.Key.bShift) { Token += "Shift+"; }
                        if (Binding.Key.bAlt)   { Token += "Alt+"; }
                        Token += FInputActionMap::KeyToString(Binding.Key.Key);
                    }
                    break;
                }

                if (Token.empty())
                {
                    continue;
                }
                if (!Result.empty())
                {
                    Result += ", ";
                }
                Result += Token;
            }
            return Result.empty() ? FString("Unbound") : Result;
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
                ImGuiX::TextTooltip_Internal(Text.c_str());
            }
        }

        bool DrawCombo(const char* Id, FString& Value, ImGuiTextFilter& Filter)
        {
            const TVector<SInputAction>& Actions = FInputActionMap::Get().GetAllActions();

            const SInputAction* Current = nullptr;
            if (!Value.empty())
            {
                Current = FInputActionMap::Get().FindAction(FName(Value.c_str()));
            }

            // A name that resolves to nothing is the rename/delete case, and reads as bound until said so.
            const bool bDangling = !Value.empty() && Current == nullptr;

            FFixedString Preview;
            if (Value.empty())
            {
                Preview = LE_ICON_CLOSE_CIRCLE_OUTLINE "  None";
            }
            else if (bDangling)
            {
                std::format_to(std::back_inserter(Preview), "{}  {}", LE_ICON_ALERT_CIRCLE_OUTLINE, Value.c_str());
            }
            else
            {
                std::format_to(std::back_inserter(Preview), "{}  {}", TypeIcon(Current->Type), Value.c_str());
            }

            if (bDangling)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Warning());
            }

            const float Scale = ImGuiX::GetUIScale();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f * Scale, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
            const bool bOpen = ImGui::BeginCombo(Id, Preview.c_str());

            if (bDangling)
            {
                ImGui::PopStyleColor();
                ImGuiX::TextTooltip("No action named '{}' in Settings > Engine > Input", Value.c_str());
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
            if (ImGui::Selectable(LE_ICON_CLOSE_CIRCLE_OUTLINE "  None", Value.empty()))
            {
                Value.clear();
                bPicked = true;
            }

            int32 Shown = 0;
            for (const SInputAction& Action : Actions)
            {
                if (Action.Name.IsNone())
                {
                    continue;
                }
                const FString ActionName(Action.Name.c_str());
                if (!ImGuiX::PassSearchFilter(Filter, ActionName.c_str()))
                {
                    continue;
                }

                ++Shown;

                FFixedString Row;
                std::format_to(std::back_inserter(Row), "{}  {}", TypeIcon(Action.Type), ActionName.c_str());
                if (ImGui::Selectable(Row.c_str(), Value == ActionName))
                {
                    Value = ActionName;
                    bPicked = true;
                }
                DrawActionRowTooltip(Action);
                DrawRightAlignedDim(TypeLabel(Action.Type));
            }

            if (Shown == 0)
            {
                ImGui::TextDisabled(Actions.empty()
                    ? "No input actions. Add them in Settings > Engine > Input."
                    : "No matching actions.");
            }

            ImGui::EndCombo();
            return bPicked;
        }
    }

    EPropertyChangeOp FInputActionHandlePropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        FString Picked = DisplayValue.Name.IsNone() ? FString() : FString(DisplayValue.Name.c_str());

        if (InputActionPicker::DrawCombo("##inputactionhandle", Picked, SearchFilter))
        {
            DisplayValue.SetName(Picked.empty() ? FName() : FName(Picked.c_str()));
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
        FInputActionHandle ActualValue;
        Property->GetValue(&ActualValue);

        // Reset-to-default, undo/redo and multi-select must win, or the next write puts our stale name back.
        if (CachedValue.Name != ActualValue.Name)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }
}
