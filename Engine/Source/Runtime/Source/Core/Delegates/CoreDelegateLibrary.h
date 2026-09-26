#pragma once

#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "CoreDelegateLibrary.generated.h"

namespace Lumina
{
    /** Addresses of the engine's process-wide events, so a script can bind one like a component event. */
    REFLECT()
    class RUNTIME_API CCoreDelegateLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        /** Fires after a world is torn down, before the next one exists. */
        FUNCTION()
        static int64 GetPostWorldUnloadEvent();

        /** Fires once as the engine begins shutting down, after which listeners are dropped. */
        FUNCTION()
        static int64 GetPreEngineShutdownEvent();

        /** Fires when gameplay asks to quit; the editor binds this to end PIE instead of exiting. */
        FUNCTION()
        static int64 GetGameQuitRequestedEvent();

        /** Fires each frame after the OS event pump, before input actions are evaluated. */
        FUNCTION()
        static int64 GetInputPumpedEvent();
    };
}
