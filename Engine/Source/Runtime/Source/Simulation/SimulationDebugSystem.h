#pragma once

#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Systems/ExclusiveSystem.h"
#include "SimulationDebugSystem.generated.h"

namespace Lumina
{
    // Draws the deterministic simulation's state while a viewport asks for it, marking its entities and flashing them on a rollback.
    REFLECT(Cosmetic)
    class RUNTIME_API CSimulationDebugSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;
        bool HasWork(const SSystemContext& Context) override;
        void OnExclusiveUpdate(const SExclusiveContext& Context) override;

    private:

        // Last frame's counters, so a change this frame can be shown as it happens.
        uint32 SeenRollbacks = 0;
        uint32 SeenResimulated = 0;
        uint32 LastRollbackDepth = 0;
    };
}
