#include "Containers/StringFormat.h"
#include <iterator>
#include "CoreTypeCustomization.h"

#include "imgui.h"
#include "Input/InputActionMap.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiKeyCapture.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace
    {
        // The whole binding as one string, such as Ctrl + Shift + K, RMB, or Unbound.
        FString KeyBindingText(const SKey& Key)
        {
            if (!Key.IsValid())
            {
                return FString("Unbound");
            }

            FString Text;
            if (Key.bCtrl)  { Text += "Ctrl + "; }
            if (Key.bShift) { Text += "Shift + "; }
            if (Key.bAlt)   { Text += "Alt + "; }

            if (Key.IsMouse())
            {
                switch (Key.MouseButton)
                {
                case EMouseKey::ButtonLeft:   Text += "LMB"; break;
                case EMouseKey::ButtonRight:  Text += "RMB"; break;
                case EMouseKey::ButtonMiddle: Text += "MMB"; break;
                default:                      Text += FInputActionMap::MouseButtonToString(Key.MouseButton); break;
                }
            }
            else
            {
                Text += FInputActionMap::KeyToString(Key.Key);
            }
            return Text;
        }
    }

    EPropertyChangeOp FKeyPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        bool bCommitted = false; // a new binding was set (or cleared) this frame

        const ImGuiStyle& St     = ImGui::GetStyle();
        const float       Height = ImGui::GetFrameHeight();
        const bool        bValid = DisplayValue.IsValid();

        // Its own button, so hit-testing is ImGui's, and hidden while listening or the click double-counts.
        const bool  bShowClear = bValid && !bCapturing;
        const float ClearW     = bShowClear ? Height + St.ItemSpacing.x : 0.0f;
        const float Avail  = ImGui::GetContentRegionAvail().x - ClearW;
        const float FieldW = Avail > 1.0f ? Avail : 1.0f;

        FFixedString Label;
        AppendFormat(Label, "{}##skey",
            bCapturing ? "Press a key..." : KeyBindingText(DisplayValue).c_str());

        if (ImGui::Button(Label.c_str(), ImVec2(FieldW, Height)) && !bCapturing)
        {
            bCapturing = true;
            bArmed     = false;
        }
        ImGuiX::TextTooltip("{}", bCapturing ? "Press a key or mouse button. Esc cancels."
                                             : "Click, then press a key or mouse button to rebind");

        if (bShowClear)
        {
            ImGui::SameLine(0.0f, St.ItemSpacing.x);
            if (ImGui::Button(LE_ICON_CLOSE "##clearskey", ImVec2(Height, Height)))
            {
                DisplayValue.Clear();
                bCapturing = false;
                bCommitted = true;
            }
            ImGuiX::TextTooltip("Clear binding");
        }

        if (bCapturing)
        {
            if (!bArmed)
            {
                // Skip the frame the activating click landed on; otherwise that click binds immediately.
                bArmed = true;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat*/ false))
            {
                bCapturing = false; // cancel, leave the binding unchanged
            }
            else
            {
                // Bare modifier presses keep listening, so a chord can be built before the key commits.
                const ImGuiIO& Io = ImGui::GetIO();
                const EKey K = ImGuiX::PollPressedKey();
                if (K != EKey::Num && !ImGuiX::IsModifierEKey(K))
                {
                    DisplayValue.SetKey(K);
                    DisplayValue.bCtrl  = Io.KeyCtrl;
                    DisplayValue.bShift = Io.KeyShift;
                    DisplayValue.bAlt   = Io.KeyAlt;
                    bCapturing = false;
                    bCommitted = true;
                }
                else if (const EMouseKey M = ImGuiX::PollPressedMouseButton(); M != EMouseKey::Num)
                {
                    DisplayValue.SetMouseButton(M);
                    DisplayValue.bCtrl  = Io.KeyCtrl;
                    DisplayValue.bShift = Io.KeyShift;
                    DisplayValue.bAlt   = Io.KeyAlt;
                    bCapturing = false;
                    bCommitted = true;
                }
            }
        }

        // A discrete-edit transaction, opened on the change frame and closed the next.
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

    void FKeyPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FKeyPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        SKey ActualValue;
        Property->GetValue(&ActualValue);

        // An external change must win even mid-listen, or the next write puts the stale value back.
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
            bCapturing  = false;
            bArmed      = false;
        }
    }
}
