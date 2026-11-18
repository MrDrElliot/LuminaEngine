#pragma once

#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Systems/ExclusiveSystem.h"
#include "NetDebugDrawSystem.generated.h"

namespace Lumina
{
    // Draws what the network layer knows while a viewport asks for it, the rewind window, each proxy's interpolation and the link's traffic.
    REFLECT(Cosmetic)
    class NETWORKINGRUNTIME_API CNetDebugDrawSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;
        bool HasWork(const SSystemContext& Context) override;
        void OnExclusiveUpdate(const SExclusiveContext& Context) override;

    private:

        void DrawLagCompensation(const SExclusiveContext& Context);
        void DrawProxies(const SExclusiveContext& Context);
        void DrawStats(const SExclusiveContext& Context);
        void DrawInterest(const SExclusiveContext& Context);

        // The transport's running totals at the last sample, so the overlay shows rates rather than totals.
        uint64 LastSentBytes = 0;
        uint64 LastReceivedBytes = 0;
        double LastSampleTime = 0.0;
        float  SentPerSecond = 0.0f;
        float  ReceivedPerSecond = 0.0f;
    };
}
