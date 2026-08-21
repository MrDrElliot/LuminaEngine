#include "NetworkingRuntimeModule.h"

#include "LuminaNetworkRuntime.h"
#include "Core/Module/ModuleManager.h"
#include "Log/Log.h"
#include "Networking/INetworkRuntime.h"

using namespace Lumina;

IMPLEMENT_MODULE(FNetworkingRuntimeModule, "NetworkingRuntime");

void FNetworkingRuntimeModule::StartupModule()
{
    SetNetworkRuntime(&FLuminaNetworkRuntime::Get());

    LOG_INFO("[Networking] Lumina netcode installed.");
}

void FNetworkingRuntimeModule::ShutdownModule()
{
    // This module owns the object the engine holds, which would outlive its code on unload.
    SetNetworkRuntime(nullptr);
}
