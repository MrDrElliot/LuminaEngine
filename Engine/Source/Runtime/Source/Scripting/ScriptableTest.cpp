#include "RuntimePCH.h"
#include "ScriptableTest.h"
#include "Simulation/Simulation.h"
#include "World/World.h"

namespace Lumina
{
    bool CWorldSubsystemTest::bAllowCreation = false;
    bool CEntitySystemTest::bAllowCreation   = false;
    int32 CDestroyCountTest::DestroyCount    = 0;
    bool CSelfDefaultTest::bAskForOwnDefault = false;

    void CSimTestSystem::OnExclusiveUpdate(const SExclusiveContext& Context)
    {
        FSimulation* Sim = Context.GetSimulation();
        if (Sim == nullptr)
        {
            return;
        }
        if (ManagedTick != nullptr)
        {
            ManagedTick(&Context);
            return;
        }

        ECS::FRegistry& Registry = Context.GetRegistry();
        if (Sim->GetTick() == 0)
        {
            for (uint32 Player = 0; Player < Sim->GetNumPlayers(); ++Player)
            {
                Registry.Emplace<SSimTestBody>(Sim->Spawn()).Owner = (int32)Player;
            }
        }

        TVector<ECS::FEntity> Entities;
        Sim->GetEntities(Entities);
        for (ECS::FEntity Entity : Entities)
        {
            SSimTestBody* Body = Registry.TryGet<SSimTestBody>(Entity);
            if (Body == nullptr)
            {
                continue;
            }

            if (Body->Owner >= 0)
            {
                const SSimTestInput& Input = Sim->GetInput<SSimTestInput>((uint32)Body->Owner);
                Body->X += Input.MoveX;
                Body->Energy = Body->Energy * 0.9f + 0.37f * (float)Input.MoveX;
                if (Input.bFire)
                {
                    const int32 FromX = Body->X;
                    SSimTestBody& Shot = Registry.Emplace<SSimTestBody>(Sim->Spawn());
                    Shot.X = FromX;
                    Shot.Energy = (float)(Sim->GetRandom().NextUInt32() % 1000) * 0.001f;
                    ++ProjectilesSpawned;
                }
            }
            else
            {
                Body->X += 3;
                Body->Energy *= 1.01f;
                if (Body->X > 120)
                {
                    Sim->Destroy(Entity);
                    continue;
                }
            }

            if (Sim->GetRandom().NextUInt32() % 7 == 0)
            {
                ++Body->Hits;
            }
        }

        if ((int32)Sim->GetTick() == CorruptAtTick && !Entities.IsEmpty())
        {
            if (SSimTestBody* Body = Registry.TryGet<SSimTestBody>(Entities[0]))
            {
                Body->X += 1000;
            }
        }
    }
}
