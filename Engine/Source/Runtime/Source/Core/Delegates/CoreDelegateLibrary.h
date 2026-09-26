#pragma once

#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "CoreDelegateLibrary.generated.h"

namespace Lumina
{
    /** Reaches the engine's process-wide events, so a script binds one like a component event. */
    REFLECT()
    class RUNTIME_API CCoreDelegateLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        /** The address of the SCoreDelegates singleton, which the managed wrapper views its slots over. */
        FUNCTION()
        static int64 GetCoreDelegates();
    };
}
