#pragma once
#include "Delegate.h"
#include "Containers/String.h"


namespace Lumina
{
    struct FModuleInfo;
    class CWorld;
    class CClass;

    struct FCoreDelegates
    {
        // Exported so applications (e.g. the Lumina launcher) and game DLLs
        // can register handlers across DLL boundaries.
        RUNTIME_API static TMulticastDelegate<void>		            OnPreEngineInit;
        RUNTIME_API static TMulticastDelegate<void>		            OnPostEngineInit;
        RUNTIME_API static TMulticastDelegate<void>                 PostWorldUnload;
        RUNTIME_API static TMulticastDelegate<void>		            OnPreEngineShutdown;
        RUNTIME_API static TMulticastDelegate<void, FModuleInfo*>   OnModuleLoaded;
        RUNTIME_API static TMulticastDelegate<void>                 OnModuleUnloaded;

        // Fired after FEngine::Travel swaps worlds; OldWorld is torn down (identity-compare
        // only). Subscribers must drop cached entity handles/property tables on OldWorld.
        RUNTIME_API static TMulticastDelegate<void, CWorld*, CWorld*> OnWorldTraveled;

        // Fired by editor file watchers on content change (arg = VFS path); subscribers
        // filter by extension. Editor-only in practice.
        RUNTIME_API static TMulticastDelegate<void, FStringView>     OnContentFileModified;

        // Fired when a tracked text asset (.luau/.rml/.rcss) is renamed/moved (args = old VFS path,
        // new VFS path). Open file editors retarget their path so a later save writes the new file.
        RUNTIME_API static TMulticastDelegate<void, FStringView, FStringView> OnContentFileRenamed;

        // Fired after FConfig::SaveSettings persists a CDeveloperSettings class (arg = that CClass*).
        // Lets open editors live-refresh from their settings instead of waiting for a reopen.
        RUNTIME_API static TMulticastDelegate<void, CClass*>         OnSettingsSaved;

        // Fired by FEngine::RequestExitGame when gameplay asks to quit. The editor binds this to end the
        // PIE session instead; when unbound (packaged game) the engine exits the process.
        RUNTIME_API static TMulticastDelegate<void>                  OnGameQuitRequested;
    };
}
