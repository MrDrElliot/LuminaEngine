#pragma once

#include "Platform/GenericPlatform.h"
#include "ModuleAPI.h"

namespace Lumina
{
    class CWorld;
    class CEntityScript;
    enum class ENetLeaveReason : uint8;
    enum class ERpcTarget : uint8;

    // What happened to a session, as scripts hear it. The first two reach the host, the last two a client.
    enum class ENetSessionEvent : uint8
    {
        ClientJoined = 0,
        ClientLeft   = 1,
        JoinedHost   = 2,
        LeftHost     = 3,
    };

    // Kept in Runtime so the netcode plugin needs no scripting headers, and each call is a no-op for a non-C# script.
    namespace NetScripts
    {
        // CallerId is the connection the RPC arrived from.
        RUNTIME_API void DispatchRpc(CEntityScript* Script, uint32 RpcId, uint32 CallerId, const uint8* Payload, uint32 PayloadSize);

        // The target the script's own declaration gives RpcId, so a relay never trusts the one a client wrote.
        RUNTIME_API bool FindRpcTarget(const CEntityScript* Script, uint32 RpcId, ERpcTarget& OutTarget);

        // A remote connection finished joining World, or left it.
        RUNTIME_API void DispatchConnection(CWorld* World, uint32 ConnectionId, bool bJoined);

        // Tells every ready script in World through its session virtuals, which a C# script overrides.
        RUNTIME_API void DispatchSession(CWorld* World, ENetSessionEvent Event, uint32 ConnectionId, ENetLeaveReason Reason);

        // Brackets a replicated update so change handlers see both values. Names are zero-terminated, back to back.
        RUNTIME_API void DispatchSyncChanging(CEntityScript* Script, const char* Names, uint32 NamesSize);
        RUNTIME_API void DispatchSyncChanged(CEntityScript* Script, const char* Names, uint32 NamesSize);
    }
}
