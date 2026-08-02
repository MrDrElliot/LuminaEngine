#include "RuntimePCH.h"
#include "ApplicationGlobalState.h"

#include "Core/Assertions/Assert.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "Memory/Memory.h"

namespace Lumina
{
    namespace
    {
        bool bGlobalStateInitialize = false;
    }
    
    FApplicationGlobalState::FApplicationGlobalState(char const* MainThreadName)
    {
        ASSERT(!bGlobalStateInitialize);
        
        Threading::Initialize(MainThreadName == nullptr ? "Main Thread" : MainThreadName);
        Logging::Init();

        bGlobalStateInitialize = true;
    }

    FApplicationGlobalState::~FApplicationGlobalState()
    {
        ASSERT(bGlobalStateInitialize);
        bGlobalStateInitialize = false;

        Logging::Shutdown();
        Threading::Shutdown();
    }
}
