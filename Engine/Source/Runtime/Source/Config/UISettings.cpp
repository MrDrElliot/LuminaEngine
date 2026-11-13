#include "RuntimePCH.h"
#include "UISettings.h"

#include "UI/RmlUiBridge.h"

namespace Lumina
{
    void CUISettings::PostInitSettings()
    {
        Super::PostInitSettings();
        RmlUi::RegisterProjectFonts();
    }

    const SHardwareCursor* CUISettings::FindHardwareCursor(EMouseCursor Cursor) const
    {
        for (const SHardwareCursor& Entry : HardwareCursors)
        {
            if (Entry.Cursor == Cursor && !Entry.Image.IsEmpty())
            {
                return &Entry;
            }
        }
        return nullptr;
    }

    void CUISettings::PostPropertyChange(FProperty* ChangedProperty)
    {
        Super::PostPropertyChange(ChangedProperty);
        RmlUi::RegisterProjectFonts();
    }
}
