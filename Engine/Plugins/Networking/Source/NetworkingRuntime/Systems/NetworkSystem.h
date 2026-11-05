#pragma once
#include "World/Entity/Systems/ExclusiveSystem.h"
#include "Systems/NetMovementInterpSystem.h"
#include "NetworkSystem.generated.h"

namespace Lumina
{
    // Per-world networking driver.
    REFLECT()
    class SNetworkSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnExclusiveUpdate(const SExclusiveContext& Context) override;
        bool HasWork(const SSystemContext& Context) override;

    private:

        TResMut<FNetInterpClock> Clock;
    };
}
