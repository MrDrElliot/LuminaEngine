#include "RuntimePCH.h"
#include "CoreDelegateLibrary.h"

#include "CoreDelegates.h"

namespace Lumina
{
    namespace
    {
        int64 AddressOf(FScriptDelegate& Event)
        {
            return (int64)reinterpret_cast<FScriptDelegateBase*>(&Event);
        }
    }

    int64 CCoreDelegateLibrary::GetPostWorldUnloadEvent()
    {
        return AddressOf(FCoreDelegates::PostWorldUnload);
    }

    int64 CCoreDelegateLibrary::GetPreEngineShutdownEvent()
    {
        return AddressOf(FCoreDelegates::OnPreEngineShutdown);
    }

    int64 CCoreDelegateLibrary::GetGameQuitRequestedEvent()
    {
        return AddressOf(FCoreDelegates::OnGameQuitRequested);
    }

    int64 CCoreDelegateLibrary::GetInputPumpedEvent()
    {
        return AddressOf(FCoreDelegates::OnInputPumped);
    }
}
