#include "RuntimePCH.h"
#include "InputSettings.h"

#include "Input/InputActionMap.h"

namespace Lumina
{
    void CInputSettings::PostInitSettings()
    {
        // Fold the pre-EInputActionType bAxis flag into Type on the settings object itself, so the Input
        // panel shows what the runtime will actually do and the next save drops the legacy flag. The same
        // fold happens in RebuildFromSettings, which covers action lists that never come through here.
        for (SInputAction& Action : Actions)
        {
            if (Action.bAxis)
            {
                if (Action.Type == EInputActionType::Digital)
                {
                    Action.Type = EInputActionType::Axis1D;
                }
                Action.bAxis = false;
            }
        }

        FInputActionMap::Get().RebuildFromSettings();
    }
}
