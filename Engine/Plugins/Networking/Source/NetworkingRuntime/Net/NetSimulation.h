#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CWorld;
    class CStruct;
    struct FNetWorldState;
    enum class ENetMessage : uint8;

    // Carries a deterministic simulation's inputs and checksums. Clients talk only to the host, which relays to everyone else.
    namespace NetSimulation
    {
        // Host only. The host is player 0 unless it is dedicated, and every ready client takes the next slot in join order.
        NETWORKINGRUNTIME_API bool StartSession(CWorld* World, FNetWorldState& State, const CStruct* InputStruct, uint64 Seed);

        // Sends what the simulation produced this frame, so it runs after the simulation has ticked.
        NETWORKINGRUNTIME_API void Pump(CWorld* World, FNetWorldState& State, bool bServer);

        // True when the message was a simulation message and has been handled.
        NETWORKINGRUNTIME_API bool HandleMessage(CWorld* World, FNetWorldState& State, bool bServer, uint32 SenderConnection, ENetMessage Type, const uint8* Data, SIZE_T Size);

        // A connection that left stops being relayed to; its player keeps predicting from the last input it sent.
        NETWORKINGRUNTIME_API void OnConnectionLeft(FNetWorldState& State, uint32 Connection);
    }
}
