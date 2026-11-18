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

        // Shared with the null-fallback at NetMovementInterpSystem's call site, so a rename can't silently diverge.
        static constexpr float kDefaultInterpDelay = 0.04f;

        /** Floor for how many seconds a SimulatedProxy renders behind the newest received server time. */
        PROPERTY(Editable, Category = "Replication", ClampMin = 0.0f, ClampMax = 1.0f)
        float InterpDelay = kDefaultInterpDelay;

        // Shared with the null-fallback at NetMovementInterpSystem's call site, so a rename can't silently diverge.
        static constexpr bool kDefaultEnableExtrapolation = false;

        /** Dead-reckon a proxy's position from its last velocity when render time runs past the newest sample. */
        PROPERTY(Editable, Category = "Replication")
        bool bEnableExtrapolation = kDefaultEnableExtrapolation;

        // Shared with the null-fallback at NetMovementInterpSystem's call site, so a rename can't silently diverge.
        static constexpr float kDefaultTeleportDistance = 3.0f;

        // A proxy that moved farther than this between two updates snaps instead of interpolating, so a teleport stays a teleport.
        PROPERTY(Editable, Category = "Replication", ClampMin = 0.0f, Units = "m")
        float TeleportDistance = kDefaultTeleportDistance;

        // Most entities one client holds at once, keeping the nearest. Zero holds everything in range.
        PROPERTY(Editable, Category = "Replication", ClampMin = 0, ClampMax = 65535)
        int32 MaxRelevantPerClient = 0;

        // Transform bytes per second one client may receive. Past it the stalest and nearest go first. Zero is unlimited.
        PROPERTY(Editable, Category = "Replication", ClampMin = 0, Units = "B/s")
        int32 MaxSnapshotBytesPerClient = 0;

        // Furthest back the host rewinds hitboxes to judge a shot. Longer forgives worse pings and lets players be hit later behind cover.
        PROPERTY(Editable, Category = "Lag Compensation", ClampMin = 0.0f, ClampMax = 1.0f, Units = "s")
        float MaxLagCompensationSeconds = 0.35f;

        // Hitbox snapshots a second, which shots interpolate between, so a fast host frame rate costs no extra memory. Zero records every frame.
        PROPERTY(Editable, Category = "Lag Compensation", ClampMin = 0.0f, ClampMax = 240.0f, Units = "Hz")
        float LagCompensationRecordRate = 60.0f;

        // How long a client keeps a predicted spawn's stand-in waiting for the host's copy before it decides the host refused.
        PROPERTY(Editable, Category = "Prediction", ClampMin = 0.1f, ClampMax = 10.0f, Units = "s")
        float PredictedSpawnTimeout = 1.0f;

        // How often each client receives snapshots, spread across server ticks. Zero sends every tick.
        PROPERTY(Editable, Category = "Replication", ClampMin = 0.0f, ClampMax = 240.0f, Units = "Hz")
        float ServerSendRate = 30.0f;

        // How often the host confirms a predicted character's state to its owner. Lower saves bandwidth and delays corrections.
        PROPERTY(Editable, Category = "Replication", ClampMin = 1.0f, ClampMax = 120.0f, Units = "Hz")
        float MoveAckRate = 20.0f;

        // Shared with the null-fallback at NetMovementInterpSystem's call site, so a rename can't silently diverge.
        static constexpr float kDefaultMaxExtrapolation = 0.25f;

        /** Maximum seconds to extrapolate past the newest sample before clamping. */
        PROPERTY(Editable, Category = "Replication", ClampMin = 0.0f, ClampMax = 1.0f)
        float MaxExtrapolation = kDefaultMaxExtrapolation;

        // Shared with the null-fallback at NetMovementInterpSystem's call site, so a rename can't silently diverge.
        static constexpr float kDefaultInterpBufferIntervals = 1.5f;

        /** Per-entity interpolation buffer depth, in multiples of the entity's measured send interval. */
        PROPERTY(Editable, Category = "Replication", ClampMin = 1.0f, ClampMax = 3.0f)
        float InterpBufferIntervals = kDefaultInterpBufferIntervals;

        // Property bytes collected per host tick across all entities. Over budget, the oldest changes go first next tick, so none starve. Zero is unlimited.
        PROPERTY(Editable, Category = "Replication|Flow Control", ClampMin = 0)
        int32 MaxPropertyBytesPerTick = 32768;

        // How long the host waits for a client to acknowledge a property packet before sending its entities again, when no later packet shows it lost.
        PROPERTY(Editable, Category = "Replication|Flow Control", ClampMin = 0.05f, ClampMax = 10.0f, Units = "s")
        float PropertyAckTimeout = 1.0f;
    };
}
