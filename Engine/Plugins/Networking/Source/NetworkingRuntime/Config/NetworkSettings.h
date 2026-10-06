#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Config/DeveloperSettings.h"
#include "NetworkSettings.generated.h"

namespace Lumina
{
    // Client-side proxy-smoothing preferences.
    REFLECT(MinimalAPI, ConfigFile = "/Config/NetworkSettings.json", DisplayName = "Networking", Category = "Engine")
    class CNetworkSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        // Most remote connections a server accepts.
        PROPERTY(Editable, Category = "Server", ClampMin = 1, ClampMax = 4000)
        int32 MaxClients = 64;

        // Seconds a client waits for its host to answer before it gives up and reports TimedOut.
        PROPERTY(Editable, Category = "Session", ClampMin = 1.0f, Units = "s")
        float ConnectTimeout = 10.0f;

        /** Floor for how many seconds a SimulatedProxy renders behind the newest received server time. */
        PROPERTY(Editable, Category = "Replication", ClampMin = 0.0f, ClampMax = 1.0f)
        float InterpDelay = 0.04f;

        /** Dead-reckon a proxy's position from its last velocity when render time runs past the newest sample. */
        PROPERTY(Editable, Category = "Replication")
        bool bEnableExtrapolation = false;

        // A proxy that moved farther than this between two updates snaps instead of interpolating, so a teleport stays a teleport.
        PROPERTY(Editable, Category = "Replication", ClampMin = 0.0f, Units = "m")
        float TeleportDistance = 3.0f;

        // Most entities one client holds at once, keeping the nearest. Zero holds everything in range.
        PROPERTY(Editable, Category = "Replication", ClampMin = 0, ClampMax = 65535)
        int32 MaxRelevantPerClient = 0;

        // Transform bytes per second one client may receive. Past it the stalest and nearest go first. Zero is unlimited.
        PROPERTY(Editable, Category = "Replication", ClampMin = 0, Units = "B/s")
        int32 MaxSnapshotBytesPerClient = 0;

        // How often each client receives snapshots, spread across server ticks. Zero sends every tick.
        PROPERTY(Editable, Category = "Replication", ClampMin = 0.0f, ClampMax = 240.0f, Units = "Hz")
        float ServerSendRate = 30.0f;

        // How often the host confirms a predicted character's state to its owner. Lower saves bandwidth and delays corrections.
        PROPERTY(Editable, Category = "Replication", ClampMin = 1.0f, ClampMax = 120.0f, Units = "Hz")
        float MoveAckRate = 20.0f;

        /** Maximum seconds to extrapolate past the newest sample before clamping. */
        PROPERTY(Editable, Category = "Replication", ClampMin = 0.0f, ClampMax = 1.0f)
        float MaxExtrapolation = 0.25f;

        /** Per-entity interpolation buffer depth, in multiples of the entity's measured send interval. */
        PROPERTY(Editable, Category = "Replication", ClampMin = 1.0f, ClampMax = 3.0f)
        float InterpBufferIntervals = 1.5f;

        /** Per-server-tick cap on reliable property-update bytes (0 = unlimited). When a burst of dirty
         *  entities would exceed this, the over-budget ones stay dirty and replicate on a later tick
         *  (oldest-first, so none starve) -- this bounds the reliable queue. Delivery is still guaranteed:
         *  the reliable channel acks + retransmits. */
        PROPERTY(Editable, Category = "Replication|Flow Control", ClampMin = 0)
        int32 MaxReliablePropertyBytesPerTick = 32768;

        /** If a client's un-acked reliable backlog reaches this many bytes, the server pauses sending NEW
         *  property updates until it drains, so a slow/congested client can't make the reliable queue grow
         *  without bound (0 = never pause). Dirty entities persist, so nothing is lost -- it just waits. */
        PROPERTY(Editable, Category = "Replication|Flow Control", ClampMin = 0)
        int32 ReliableBacklogPauseBytes = 65536;
    };
}
