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
    // Cleared unconditionally: this module owns the object the engine holds, and leaving the pointer
    // behind would outlive the code it points at the moment this DLL unloads.
    SetNetworkRuntime(nullptr);
}
