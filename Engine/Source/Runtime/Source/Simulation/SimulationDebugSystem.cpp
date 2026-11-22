#include "RuntimePCH.h"
#include "SimulationDebugSystem.h"

#include "Simulation/Simulation.h"
#include "Simulation/SimulationComponents.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/DebugDrawSystem.h"
#include "World/Subsystems/DebugSubsystem.h"
#include "World/World.h"

namespace Lumina
{
    void CSimulationDebugSystem::Configure()
    {
        #if !defined(LE_SHIPPING)
        RequireUpdate(EUpdateStage::FrameEnd);
        #endif
    }

    bool CSimulationDebugSystem::HasWork(const SSystemContext& Context)
    {
        CWorld* World = GetOwningWorld();
        const FDebugDrawState* State = DebugDraw::GetState(World);
        return State != nullptr && State->Draws(EDebugDrawNetworking::Simulation) && World->GetSimulation() != nullptr;
    }

    void CSimulationDebugSystem::OnExclusiveUpdate(const SExclusiveContext& Context)
    {
        CWorld* World = Context.GetWorld();
        FSimulation* Sim = World->GetSimulation();
        CDebugSubsystem* Draw = World->GetDebug();
        if (Sim == nullptr || Draw == nullptr)
        {
            return;
        }

        if (!Sim->IsRunning())
        {
            Draw->DrawText("Simulation idle, no session started", FVector4(0.7f, 0.7f, 0.7f, 1.0f));
            return;
        }

        const bool bRolledBack = Sim->GetRollbackCount() != SeenRollbacks;
        if (bRolledBack)
        {
            LastRollbackDepth = Sim->GetResimulatedTicks() - SeenResimulated;
        }
        SeenRollbacks   = Sim->GetRollbackCount();
        SeenResimulated = Sim->GetResimulatedTicks();

        const FVector4 Text = Sim->HasDesynced() ? FVector4(1.0f, 0.3f, 0.3f, 1.0f) : FVector4(0.6f, 1.0f, 0.7f, 1.0f);
        Draw->DrawText(Format("Simulation {} | player {} of {} | tick {} | confirmed {} | ahead {}",
            Sim->GetMode() == ESimulationMode::Rollback ? "rollback" : "lockstep", Sim->GetLocalPlayer(), Sim->GetNumPlayers(),
            Sim->GetCurrentTick(), Sim->GetConfirmedTick(), (int64)Sim->GetCurrentTick() - Sim->GetConfirmedTick()), Text);
        Draw->DrawText(Format("Rollbacks {} | replayed ticks {} | last rewind {} ticks | checksums verified {}",
            Sim->GetRollbackCount(), Sim->GetResimulatedTicks(), LastRollbackDepth, Sim->GetVerifiedChecksums()), Text);
        if (Sim->HasDesynced())
        {
            Draw->DrawText(Format("Desync at tick {}, see the log for the component that diverged", Sim->GetDesyncTick()), FVector4(1.0f, 0.2f, 0.2f, 1.0f));
        }

        // Orange on the frame a rollback rewrote them, red once the peers disagree.
        const FVector4 Marker = Sim->HasDesynced() ? FVector4(1.0f, 0.15f, 0.15f, 1.0f)
                              : bRolledBack ? FVector4(1.0f, 0.55f, 0.1f, 1.0f) : FVector4(0.3f, 0.9f, 1.0f, 1.0f);
        Context.GetRegistry().View<SSimEntity, STransformComponent>().ForEach([&](ECS::FEntity, const SSimEntity&, const STransformComponent& Transform)
        {
            Draw->DrawPoint(Transform.GetWorldLocationCached(), 0.25f, Marker, 2.0f, -1.0f, false);
        });
    }
}
