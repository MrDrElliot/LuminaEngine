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

    void CUISettings::PostPropertyChange(FProperty* ChangedProperty)
    {
        Super::PostPropertyChange(ChangedProperty);
        RmlUi::RegisterProjectFonts();
    }
}
