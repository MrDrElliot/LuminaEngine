#include "RuntimePCH.h"
#include "SimulationLibrary.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Networking/INetworkRuntime.h"
#include "Simulation/Simulation.h"
#include "World/World.h"

namespace Lumina
{
    namespace SimulationLibraryInternal
    {
        FSimulation* RunningOf(const SSystemContext& Context)
        {
            FSimulation* Sim = Context.GetSimulation();
            return Sim != nullptr && Sim->IsRunning() ? Sim : nullptr;
        }

        FSimulation* RunningOf(CWorld* World)
        {
            FSimulation* Sim = World != nullptr ? World->GetSimulation() : nullptr;
            return Sim != nullptr && Sim->IsRunning() ? Sim : nullptr;
        }

        bool IsInputStruct(const FSimulation& Sim, FName InputStruct)
        {
            const CStruct* Struct = Sim.GetInputStruct();
            if (Struct != nullptr && Struct->GetName() == InputStruct)
            {
                return true;
            }
            LOG_ERROR("Simulation: the session's input is {}, not {}.", Struct != nullptr ? Struct->GetName().ToString() : FString("nothing"), InputStruct.ToString());
            return false;
        }

        // Spawning and destroying change the registry at once, which only a system running alone may do.
        bool CanChangeStructure(const char* What)
        {
            if (ECS::FCommandBus::ShouldDefer())
            {
                LOG_ERROR("Simulation: {} needs an exclusive system, since a parallel one runs beside others.", What);
                return false;
            }
            return true;
        }
    }

    using namespace SimulationLibraryInternal;

    bool CSimulationLibrary::IsRunning(const SSystemContext& Context)
    {
        return RunningOf(Context) != nullptr;
    }

    uint32 CSimulationLibrary::GetTick(const SSystemContext& Context)
    {
        const FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr ? Sim->GetTick() : 0u;
    }

    float CSimulationLibrary::GetTickSeconds(const SSystemContext& Context)
    {
        const FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr ? Sim->GetTickSeconds() : 0.0f;
    }

    bool CSimulationLibrary::IsResimulating(const SSystemContext& Context)
    {
        const FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr && Sim->IsResimulating();
    }

    uint32 CSimulationLibrary::GetNumPlayers(const SSystemContext& Context)
    {
        const FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr ? Sim->GetNumPlayers() : 0u;
    }

    uint32 CSimulationLibrary::GetLocalPlayer(const SSystemContext& Context)
    {
        const FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr ? Sim->GetLocalPlayer() : 0u;
    }

    void* CSimulationLibrary::GetInput(const SSystemContext& Context, uint32 Player, FName InputStruct)
    {
        const FSimulation* Sim = RunningOf(Context);
        if (Sim == nullptr || !IsInputStruct(*Sim, InputStruct))
        {
            return nullptr;
        }
        return const_cast<void*>(Sim->GetInput(Player));
    }

    ECS::FEntity CSimulationLibrary::Spawn(const SSystemContext& Context)
    {
        FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr && CanChangeStructure("Spawn") ? Sim->Spawn() : ECS::NullEntity;
    }

    void CSimulationLibrary::Destroy(const SSystemContext& Context, ECS::FEntity Entity)
    {
        FSimulation* Sim = RunningOf(Context);
        if (Sim != nullptr && CanChangeStructure("Destroy"))
        {
            Sim->Destroy(Entity);
        }
    }

    ECS::FEntity CSimulationLibrary::FindEntity(const SSystemContext& Context, uint32 SimId)
    {
        const FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr ? Sim->FindEntity(SimId) : ECS::NullEntity;
    }

    void CSimulationLibrary::GetEntities(const SSystemContext& Context, TVector<ECS::FEntity>& Out)
    {
        if (const FSimulation* Sim = RunningOf(Context))
        {
            Sim->GetEntities(Out);
        }
    }

    uint32 CSimulationLibrary::RandomUInt32(const SSystemContext& Context)
    {
        FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr ? Sim->GetRandom().NextUInt32() : 0u;
    }

    int32 CSimulationLibrary::RandomRange(const SSystemContext& Context, int32 Min, int32 Max)
    {
        FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr ? Sim->GetRandom().RandRange(Min, Max) : Min;
    }

    float CSimulationLibrary::RandomFloat(const SSystemContext& Context)
    {
        FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr ? Sim->GetRandom().NextFloat() : 0.0f;
    }

    float CSimulationLibrary::RandomFloatRange(const SSystemContext& Context, float Min, float Max)
    {
        FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr ? Sim->GetRandom().RandRange(Min, Max) : Min;
    }

    bool CSimulationLibrary::RandomBool(const SSystemContext& Context)
    {
        FSimulation* Sim = RunningOf(Context);
        return Sim != nullptr && Sim->GetRandom().RandBool();
    }

    bool CSimulationLibrary::StartSession(CWorld* World, FName InputStruct, uint64 Seed)
    {
        if (World == nullptr)
        {
            return false;
        }

        const CStruct* Struct = FindObject<CStruct>(InputStruct);
        if (Struct == nullptr)
        {
            LOG_ERROR("Simulation: no struct named {} to use as the session's input.", InputStruct.ToString());
            return false;
        }

        switch (World->GetNetMode())
        {
        case ENetMode::Standalone:
            {
                FSimulation* Sim = World->GetSimulation();
                if (Sim == nullptr)
                {
                    LOG_ERROR("Simulation: this world has no simulation. Enable Deterministic Simulation in the project settings.");
                    return false;
                }
                FSimSessionDesc Desc = FSimSessionDesc::FromProjectSettings();
                Desc.InputStruct = Struct;
                Desc.Seed        = Seed;
                return Sim->Start(Desc);
            }
        case ENetMode::Client:
            LOG_ERROR("Simulation: a client joins the session its host starts, so it cannot start one.");
            return false;
        default:
            {
                INetworkRuntime* Runtime = GetNetworkRuntime();
                return Runtime != nullptr && Runtime->StartSimulationSession(World, Struct, Seed);
            }
        }
    }

    bool CSimulationLibrary::IsSessionRunning(CWorld* World)
    {
        return RunningOf(World) != nullptr;
    }

    ESimulationMode CSimulationLibrary::GetMode(CWorld* World)
    {
        const FSimulation* Sim = RunningOf(World);
        return Sim != nullptr ? Sim->GetMode() : ESimulationMode::Rollback;
    }

    uint32 CSimulationLibrary::GetSessionPlayers(CWorld* World)
    {
        const FSimulation* Sim = RunningOf(World);
        return Sim != nullptr ? Sim->GetNumPlayers() : 0u;
    }

    uint32 CSimulationLibrary::GetSessionLocalPlayer(CWorld* World)
    {
        const FSimulation* Sim = RunningOf(World);
        return Sim != nullptr ? Sim->GetLocalPlayer() : 0u;
    }

    void* CSimulationLibrary::GetLocalInput(CWorld* World, FName InputStruct)
    {
        FSimulation* Sim = RunningOf(World);
        return Sim != nullptr && IsInputStruct(*Sim, InputStruct) ? Sim->GetLocalInput() : nullptr;
    }

    float CSimulationLibrary::GetInterpolationAlpha(CWorld* World)
    {
        const FSimulation* Sim = RunningOf(World);
        return Sim != nullptr ? Sim->GetInterpolationAlpha() : 0.0f;
    }

    uint32 CSimulationLibrary::GetCurrentTick(CWorld* World)
    {
        const FSimulation* Sim = RunningOf(World);
        return Sim != nullptr ? Sim->GetCurrentTick() : 0u;
    }

    int64 CSimulationLibrary::GetConfirmedTick(CWorld* World)
    {
        const FSimulation* Sim = RunningOf(World);
        return Sim != nullptr ? Sim->GetConfirmedTick() : -1;
    }

    uint32 CSimulationLibrary::GetRollbackCount(CWorld* World)
    {
        const FSimulation* Sim = RunningOf(World);
        return Sim != nullptr ? Sim->GetRollbackCount() : 0u;
    }

    bool CSimulationLibrary::HasDesynced(CWorld* World)
    {
        const FSimulation* Sim = RunningOf(World);
        return Sim != nullptr && Sim->HasDesynced();
    }
}
