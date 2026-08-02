#include "RuntimePCH.h"
#include "CoreDelegates.h"

namespace Lumina
{
    TMulticastDelegate<void>                    FCoreDelegates::OnPreEngineInit;
    TMulticastDelegate<void>		            FCoreDelegates::OnPostEngineInit;
    TMulticastDelegate<void>                    FCoreDelegates::PostWorldUnload;
    TMulticastDelegate<void>		            FCoreDelegates::OnPreEngineShutdown;
    TMulticastDelegate<void, FModuleInfo*>      FCoreDelegates::OnModuleLoaded;
    TMulticastDelegate<void>                    FCoreDelegates::OnModuleUnloaded;
    TMulticastDelegate<void, CWorld*, CWorld*>  FCoreDelegates::OnWorldTravelled;
    TMulticastDelegate<void, FStringView>       FCoreDelegates::OnContentFileModified;
    TMulticastDelegate<void, FStringView, FStringView> FCoreDelegates::OnContentFileRenamed;
    TMulticastDelegate<void, CClass*>           FCoreDelegates::OnSettingsSaved;
    TMulticastDelegate<void>                    FCoreDelegates::OnGameQuitRequested;
}