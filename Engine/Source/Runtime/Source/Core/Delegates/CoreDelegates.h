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
        PROPERTY(Hidden)
        FScriptDelegate PostWorldUnload;

        /** Fired once as the engine begins shutting down, after which listeners are dropped. */
        PROPERTY(Hidden)
        FScriptDelegate OnPreEngineShutdown;

        // Fired when gameplay asks to quit. The editor ends the PIE session, and when unbound the engine exits the process.
        PROPERTY(Hidden)
        FScriptDelegate OnGameQuitRequested;

        // Fired each frame after the OS event pump and before input actions, so synthetic input here acts like a real key.
        PROPERTY(Hidden)
        FScriptDelegate OnInputPumped;
    };

    struct FCoreDelegates
    {
        /** The reflected events, whose slots are what a script resolves its bindings against. */
        RUNTIME_API static SCoreDelegates& Get();

        // Exported so applications and game DLLs can register handlers across DLL boundaries.
        RUNTIME_API static TMulticastDelegate<void>		            OnPreEngineInit;
        RUNTIME_API static TMulticastDelegate<void>		            OnPostEngineInit;
        RUNTIME_API static TMulticastDelegate<void, FModuleInfo*>   OnModuleLoaded;
        RUNTIME_API static TMulticastDelegate<void>                 OnModuleUnloaded;
        // First thing in engine shutdown, while every system is still alive, for work that must stop before any of it goes.
        RUNTIME_API static TMulticastDelegate<void>                 OnEngineShutdownStarted;

        // A reflected object argument is a strong pointer that would keep the torn-down OldWorld alive, so these stay native-only.
        RUNTIME_API static TMulticastDelegate<void, CWorld*, CWorld*> OnWorldTraveled;
        RUNTIME_API static TMulticastDelegate<void, FStringView>     OnContentFileModified;
        RUNTIME_API static TMulticastDelegate<void, FStringView, FStringView> OnContentFileRenamed;
        RUNTIME_API static TMulticastDelegate<void, CClass*>         OnSettingsSaved;
        // Before a C# reload replaces its classes, so a holder the reinstancer cannot see lets go of their instances.
        RUNTIME_API static TMulticastDelegate<void>                 OnScriptsWillReload;
    };
}
