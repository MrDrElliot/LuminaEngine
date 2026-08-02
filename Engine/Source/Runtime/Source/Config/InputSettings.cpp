#include "RuntimePCH.h"
#include "InputSettings.h"

#include "Input/InputActionMap.h"

namespace Lumina
{
    void CInputSettings::PostInitSettings()
    {
        FInputActionMap::Get().RebuildFromSettings();
    }
}
