#include "RuntimePCH.h"
#include "InputSettings.h"

#include "Input/InputActionMap.h"

namespace Lumina
{
    void CInputSettings::PostInitSettings()
    {
        // The same fold happens in RebuildFromSettings, covering action lists that never come through here.
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
