#include "EditorSettings.h"

#include "Tools/UI/ImGui/ImGuiKeyCapture.h"

namespace Lumina
{
    bool SShortcut::IsPressed(ImGuiKeyChord ExtraMods, ImGuiInputFlags Flags) const
    {
        return ImGuiX::IsShortcutPressed(Primary, ExtraMods, Flags) || ImGuiX::IsShortcutPressed(Alternate, ExtraMods, Flags);
    }

    FString SShortcut::GetLabel() const
    {
        return Primary.IsValid() ? Primary.GetDisplayName() : FString();
    }
}
