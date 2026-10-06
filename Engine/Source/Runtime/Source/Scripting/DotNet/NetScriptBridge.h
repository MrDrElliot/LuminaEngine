#pragma once

#include "Platform/GenericPlatform.h"
#include "ModuleAPI.h"

namespace Lumina
{
    class CWorld;
    class CEntityScript;

    // Kept in Runtime so the netcode plugin needs no scripting headers, and each call is a no-op for a non-C# script.
    namespace NetScripts
    {
        // CallerId is the connection the RPC arrived from.
        RUNTIME_API void DispatchRpc(CEntityScript* Script, uint32 RpcId, uint32 CallerId, const uint8* Payload, uint32 PayloadSize);

        // A remote connection finished joining World, or left it.
        RUNTIME_API void DispatchConnection(CWorld* World, uint32 ConnectionId, bool bJoined);

        // Brackets a replicated update so change handlers see both values. Names are zero-terminated, back to back.
        RUNTIME_API void DispatchSyncChanging(CEntityScript* Script, const char* Names, uint32 NamesSize);
        RUNTIME_API void DispatchSyncChanged(CEntityScript* Script, const char* Names, uint32 NamesSize);
    }
}
