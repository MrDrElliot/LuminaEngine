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

        /** Floor for how many seconds a SimulatedProxy renders behind the newest received server time. */
        PROPERTY(Editable, Category = "Replication", ClampMin = 0.0f, ClampMax = 1.0f)
        float InterpDelay = 0.04f;

        /** Dead-reckon a proxy's position from its last velocity when render time runs past the newest sample. */
        PROPERTY(Editable, Category = "Replication")
        bool bEnableExtrapolation = false;

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
