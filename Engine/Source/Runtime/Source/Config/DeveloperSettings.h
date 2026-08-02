#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/Class.h"
#include "DeveloperSettings.generated.h"

namespace Lumina
{
    // Base for reflection-driven settings objects.
    REFLECT()
    class RUNTIME_API CDeveloperSettings : public CObject
    {
        GENERATED_BODY()

    public:

        // Called after this object's values are loaded from disk. Override to derive cached state.
        virtual void PostInitSettings();
    };
}
