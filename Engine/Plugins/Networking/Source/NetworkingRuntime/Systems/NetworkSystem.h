#pragma once
#include "World/Entity/Systems/EntitySystem.h"
#include "NetworkSystem.generated.h"

namespace Lumina
{
    // Per-world networking driver.
    REFLECT()
    class SNetworkSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate() override;
        bool HasWork(EUpdateStage Stage) override;
    };
}
