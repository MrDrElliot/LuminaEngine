#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Config/DeveloperSettings.h"
#include "SimulationSettings.generated.h"

namespace Lumina
{
    REFLECT()
    enum class ESimulationMode : uint8
    {
        // Runs ahead on predicted remote input and rewinds when a real one differs, for responsive combat.
        Rollback,
        // Waits for every player's input before a tick runs, so nothing is ever predicted or replayed.
        Lockstep,
    };

    // Deterministic simulation for lockstep and rollback multiplayer, off unless a project opts in.
    REFLECT(MinimalAPI, ConfigFile = "/Config/SimulationSettings.json", DisplayName = "Deterministic Simulation", Category = "Project")
    class CSimulationSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        // Gives every world a simulation that runs the Simulation stage at a fixed tick from player inputs alone.
        PROPERTY(Editable, Category = "Simulation")
        bool bEnabled = false;

        PROPERTY(Editable, Category = "Simulation")
        ESimulationMode Mode = ESimulationMode::Rollback;

        PROPERTY(Editable, Category = "Simulation", ClampMin = 10, ClampMax = 240, Units = "Hz")
        int32 TickRate = 60;

        // Ticks a local input waits before it runs, which hides that much latency without any rollback.
        PROPERTY(Editable, Category = "Simulation", ClampMin = 0, ClampMax = 15)
        int32 InputDelayTicks = 2;

        // Furthest a rollback simulation runs ahead of the last tick every player's input is known for.
        PROPERTY(Editable, Category = "Simulation", ClampMin = 1, ClampMax = 30)
        int32 MaxRollbackTicks = 8;

        // Ticks between state checksums exchanged to catch a desync. Zero turns the check off.
        PROPERTY(Editable, Category = "Simulation", ClampMin = 0, ClampMax = 600)
        int32 ChecksumInterval = 10;
    };
}
