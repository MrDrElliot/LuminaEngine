#include "RuntimePCH.h"
#include "CoreDelegates.h"

namespace Lumina
{
    TMulticastDelegate<void>                    FCoreDelegates::OnPreEngineInit;
    TMulticastDelegate<void>		            FCoreDelegates::OnPostEngineInit;
    TMulticastDelegate<void, FModuleInfo*>      FCoreDelegates::OnModuleLoaded;
    TMulticastDelegate<void>                    FCoreDelegates::OnModuleUnloaded;
    TMulticastDelegate<void, CWorld*, CWorld*>  FCoreDelegates::OnWorldTraveled;
    TMulticastDelegate<void, FStringView>       FCoreDelegates::OnContentFileModified;
    TMulticastDelegate<void, FStringView, FStringView> FCoreDelegates::OnContentFileRenamed;
    TMulticastDelegate<void, CClass*>           FCoreDelegates::OnSettingsSaved;

    SCoreDelegates& FCoreDelegates::Get()
    {
        static SCoreDelegates Events;
        return Events;
    }
}
