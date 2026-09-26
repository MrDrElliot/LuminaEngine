#pragma once
#include "Delegate.h"
#include "ScriptDelegate.h"
#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "CoreDelegates.generated.h"


namespace Lumina
{
    struct FModuleInfo;
    class CWorld;
    class CClass;

    /** The engine's process-wide events. Reflected, so a script binds one exactly as it binds a component's. */
    REFLECT()
    struct RUNTIME_API SCoreDelegates
    {
        GENERATED_BODY()

        /** Fired after a world is torn down, before the next one exists. */
        PROPERTY()
        FScriptDelegate PostWorldUnload;

        /** Fired once as the engine begins shutting down, after which listeners are dropped. */
        PROPERTY()
        FScriptDelegate OnPreEngineShutdown;

        // Fired by FEngine::RequestExitGame when gameplay asks to quit. The editor binds this to end the
        // PIE session instead; when unbound (packaged game) the engine exits the process.
        PROPERTY()
        FScriptDelegate OnGameQuitRequested;

        // Fired each frame right after the OS event pump, before input actions are evaluated. Synthetic input
        // delivered here takes the same path and the same frame as a real key.
        PROPERTY()
        FScriptDelegate OnInputPumped;
    };

    struct FCoreDelegates
    {
        /** The reflected events, whose slots are what a script resolves its bindings against. */
        RUNTIME_API static SCoreDelegates& Get();

        // Exported so applications (e.g. the Lumina launcher) and game DLLs
        // can register handlers across DLL boundaries.
        RUNTIME_API static TMulticastDelegate<void>		            OnPreEngineInit;
        RUNTIME_API static TMulticastDelegate<void>		            OnPostEngineInit;
        RUNTIME_API static TMulticastDelegate<void, FModuleInfo*>   OnModuleLoaded;
        RUNTIME_API static TMulticastDelegate<void>                 OnModuleUnloaded;

        // A reflected object argument is a strong TObjectPtr, which would keep the torn-down OldWorld
        // alive across the handler, so these stay native-only until a weak reflected form exists.
        RUNTIME_API static TMulticastDelegate<void, CWorld*, CWorld*> OnWorldTraveled;
        RUNTIME_API static TMulticastDelegate<void, FStringView>     OnContentFileModified;
        RUNTIME_API static TMulticastDelegate<void, FStringView, FStringView> OnContentFileRenamed;
        RUNTIME_API static TMulticastDelegate<void, CClass*>         OnSettingsSaved;
    };
}
