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
        // The whole binding as one string: "Ctrl + Shift + K", "RMB", "Unbound".
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

    EPropertyChangeOp FKeyPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        bool bCommitted = false; // a new binding was set (or cleared) this frame

        const ImGuiStyle& St     = ImGui::GetStyle();
        const float       Height = ImGui::GetFrameHeight();
        const bool        bValid = DisplayValue.IsValid();

        // Clear is its own button beside the field rather than a sub-region inside it, so hover and
        // hit-testing are ImGui's instead of a mouse-x compare against a hand-computed rect. Hidden
        // while listening: the click would land on the button AND be polled as an LMB binding.
        const bool  bShowClear = bValid && !bCapturing;
        const float ClearW     = bShowClear ? Height + St.ItemSpacing.x : 0.0f;
        const float Avail  = ImGui::GetContentRegionAvail().x - ClearW;
        const float FieldW = Avail > 1.0f ? Avail : 1.0f;

        FFixedString Label;
        std::format_to(std::back_inserter(Label), "{}##skey",
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
                // Commit only on a real (non-modifier) key or a mouse button, reading the modifiers
                // held at that instant. This is the standard key-binder flow: hold Ctrl/Shift/Alt and
                // then press the key. Bare modifier presses keep listening so a chord can be built.
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

        // Discrete-edit transaction: open on the change frame, close the next.
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

        // An external change (reset-to-default, undo/redo, multi-select) must win, even mid-listen:
        // otherwise the next UpdatePropertyValue writes our stale DisplayValue back and undoes it.
        // While listening with no external change, ActualValue == CachedValue, so capture isn't disturbed.
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
            bCapturing  = false;
            bArmed      = false;
        }
    }
}
