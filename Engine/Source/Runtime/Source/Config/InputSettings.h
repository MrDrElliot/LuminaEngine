#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Config/DeveloperSettings.h"
#include "Containers/Array.h"
#include "Input/InputAction.h"
#include "InputSettings.generated.h"

namespace Lumina
{
    // Project-wide input action mappings. Edited in the Settings panel (Engine > Input) and persisted
    // to the project's /Config/InputSettings.json; FInputActionMap reads its Actions at runtime.
    REFLECT(MinimalAPI, ConfigFile = "/Config/InputSettings.json", DisplayName = "Input", Category = "Engine")
    class CInputSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** Named gameplay inputs and the keys/buttons that fire them. */
        PROPERTY(Editable, Category = "Action Mappings")
        TVector<SInputAction> Actions;

        // Push the loaded actions into the live FInputActionMap.
        virtual void PostInitSettings() override;
    };
}
