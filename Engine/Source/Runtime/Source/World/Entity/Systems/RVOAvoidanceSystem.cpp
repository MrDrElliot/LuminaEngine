#include "RuntimePCH.h"
#include "RVOAvoidanceSystem.h"
#include "NavMeshSystem.h"
#include "PathFollowSystem.h"

#include "AI/Avoidance/ORCA.h"
#include "AI/Navigation/NavMesh.h"
#include "Core/Console/ConsoleVariable.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina
{
    static TConsoleVar<bool> CVarRVOEnabled("AI.RVO.Enabled", true,
        "Solve reciprocal local avoidance. Off passes the preferred velocity through untouched.");

    static TConsoleVar<bool> CVarRVODrawDebug("AI.RVO.DrawDebug", false,
        "Draw preferred and solved velocities for every avoidance agent, ignoring the per-agent flag.");

    namespace RVOAvoidanceSystemInternal
    {
        constexpr uint32 kExtractGrain = 256;

        constexpr FVector4 GPreferredColor(0.4f, 0.8f, 1.0f, 1.0f);
        constexpr FVector4 GSolvedColor(1.0f, 0.75f, 0.2f, 1.0f);
        constexpr FVector3 GDebugLift(0.0f, 0.15f, 0.0f);
    }

    void SRVOAvoidanceSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        // The preferred velocity it avoids around is what SPathFollowSystem writes.
        RunsAfter<SPathFollowSystem>();
    }

    bool SRVOAvoidanceSystem::HasWork(const SSystemContext& Context)
    {
        return AnyQueryHasRows(Avoiders);
    }

    void SRVOAvoidanceSystem::OnUpdate(const SSystemContext& Context)
    {
        using namespace RVOAvoidanceSystemInternal;

        State.bEnabled = CVarRVOEnabled.GetValue();
        State.LastAgentCount = 0;
        State.LastSolvedCount = 0;
        State.LastNavClamped = 0;

        const auto AvoiderView = Avoiders.View();
        const int32 DenseSize = AvoiderView.NumDenseSlots();
        if (DenseSize == 0)
        {
            return;
        }

        const float DeltaTime   = Context.GetDeltaTime();
        const float InvTimeStep = DeltaTime > 1e-4f ? 1.0f / DeltaTime : 60.0f;
        const bool  bForceDebug = CVarRVODrawDebug.GetValue();

        Avoidance::FCrowdAgents& Agents = State.Agents;
        Agents.Reset(DenseSize);
        State.Entities.SetNum(DenseSize);

        const FSignificanceState& Significance = *SignificanceState;

        ++State.FrameCounter;
        const uint32 Frame = State.FrameCounter;

        // Compacted first, because the dense array carries tombstones and the solve indexes by position.
        int32 Count = 0;
        AvoiderView.ForEachEntity([&](ECS::FEntity Entity)
        {
            State.Entities[Count++] = Entity;
        });

        if (Count == 0)
        {
            return;
        }

        const bool bSolveEnabled = State.bEnabled;

        const auto Extract = [&](int32 i)
        {
            const ECS::FEntity  Entity = State.Entities[i];
            SRVOAgentComponent& Agent  = AvoiderView.Get<ECS::Write<SRVOAgentComponent>>(Entity);

            const FVector3 Position = AvoiderView.Get<ECS::Read<STransformComponent>>(Entity).GetWorldLocationCached();

            float CharacterRadius = 0.5f;
            if (const SCharacterPhysicsComponent* Character = Characters.TryGet<SCharacterPhysicsComponent>(Entity))
            {
                CharacterRadius = Character->Radius;
            }

            float    Speed = 5.0f;
            FVector3 Velocity(0.0f);
            if (const SCharacterMovementComponent* Movement = Movers.TryGet<SCharacterMovementComponent>(Entity))
            {
                Speed    = Movement->MoveSpeed;
                Velocity = Movement->Velocity;
            }

            const float Radius   = Agent.Radius > 0.0f ? Agent.Radius : CharacterRadius;
            const float MaxSpeed = Agent.MaxSpeed > 0.0f ? Agent.MaxSpeed : Speed;
            const float Horizon  = Math::Max(Agent.TimeHorizon, 0.1f);

            const float Range = Agent.NeighborDistance > 0.0f
                ? Agent.NeighborDistance
                : Radius * 2.0f + MaxSpeed * Horizon;

            const uint8 Interval = Math::Max(Significance.Get(Entity).TickInterval, (uint8)1);

            // Staggered by entity index so a band re-solves a slice per frame rather than all at once.
            const bool bSolve = Interval <= 1 || ((Frame + Entity.GetIndex()) % (uint32)Interval) == 0;

            const int32 Neighbors = Math::Clamp(Agent.MaxNeighbors / (int32)Interval, 2,
                                                Math::Min(Agent.MaxNeighbors, Avoidance::kMaxORCALines));

            float PrefX = Agent.PreferredVelocity.x;
            float PrefZ = Agent.PreferredVelocity.z;
            if (PrefX != 0.0f || PrefZ != 0.0f)
            {
                // Skipped while idle, or a standing agent would creep off its spot.
                Avoidance::ApplySymmetryBreak(Entity.GetIndex(), MaxSpeed * Agent.SymmetryBreaking, PrefX, PrefZ);
            }

            uint8 Flags = 0;
            Flags |= bSolve ? Avoidance::CrowdFlag::Solve : 0;
            Flags |= (Agent.bIgnoreNeighbors || !bSolveEnabled) ? Avoidance::CrowdFlag::Passthrough : 0;
            Flags |= (bSolve && Agent.bClampToNavMesh) ? Avoidance::CrowdFlag::NavClamp : 0;

            Agents.PosX[i]           = Position.x;
            Agents.PosZ[i]           = Position.z;
            Agents.VelX[i]           = Velocity.x;
            Agents.VelZ[i]           = Velocity.z;
            Agents.PrefVelX[i]       = PrefX;
            Agents.PrefVelZ[i]       = PrefZ;
            Agents.Radius[i]         = Radius;
            Agents.MaxSpeed[i]       = MaxSpeed;
            Agents.InvTimeHorizon[i] = 1.0f / Horizon;
            Agents.NeighborRange[i]  = Range;
            Agents.Responsibility[i] = Math::Clamp(Agent.Responsibility, 0.0f, 1.0f);
            Agents.MaxNeighbors[i]   = Neighbors;
            Agents.Flags[i]          = Flags;

            // Held from the previous solve, so an agent skipping this frame keeps a sane output.
            Agents.OutVelX[i]      = Agent.AvoidanceVelocity.x;
            Agents.OutVelZ[i]      = Agent.AvoidanceVelocity.z;
            Agents.OutNeighbors[i] = Agent.LastNeighborCount;
        };

        Jobs::ParallelForOrSerial((uint32)Count, (uint32)Avoidance::kCrowdParallelThreshold, [&](uint32 Index)
        {
            Extract((int32)Index);
        }, kExtractGrain);

        Avoidance::SolveCrowd(Agents, Count, InvTimeStep, true);

        // Gathered rather than tested inline, so the raycast pass is empty unless someone opted in.
        State.NavClampList.Reset();
        FNavMesh* const NavMesh = Nav::GetReadyNavMesh(NavMeshes);
        if (NavMesh != nullptr)
        {
            for (int32 i = 0; i < Count; ++i)
            {
                if ((Agents.Flags[i] & Avoidance::CrowdFlag::NavClamp) != 0)
                {
                    State.NavClampList.Add(i);
                }
            }
        }

        if (!State.NavClampList.IsEmpty())
        {
            const float Lookahead = DeltaTime > 1e-4f ? DeltaTime : (1.0f / 60.0f);

            const auto Clamp = [&](int32 Slot)
            {
                const int32 i = State.NavClampList[Slot];

                const float SolvedX = Agents.OutVelX[i];
                const float SolvedZ = Agents.OutVelZ[i];
                if (SolvedX == 0.0f && SolvedZ == 0.0f)
                {
                    return;
                }

                const FVector3 From(Agents.PosX[i], 0.0f, Agents.PosZ[i]);
                const FVector3 To(From.x + SolvedX * Lookahead, 0.0f, From.z + SolvedZ * Lookahead);

                FNavQueryFilter   Filter;
                SNavRaycastResult Hit;
                if (NavMesh->Raycast(From, To, Filter, Hit) && Hit.bHit)
                {
                    // The preferred velocity came off a navmesh path, so it is the safe thing to fall back to.
                    Agents.OutVelX[i] = Agents.PrefVelX[i];
                    Agents.OutVelZ[i] = Agents.PrefVelZ[i];
                }
            };

            const int32 NumClamped = State.NavClampList.Num();
            Jobs::ParallelForOrSerial((uint32)NumClamped, (uint32)Avoidance::kCrowdParallelThreshold, [&](uint32 Slot)
            {
                Clamp((int32)Slot);
            }, 32);

            State.LastNavClamped = NumClamped;
        }

        std::atomic<int32> SolvedTotal{0};

        const auto DrawAgentDebug = [&](int32 i, const SRVOAgentComponent& Agent)
        {
            const FVector3 Base = AvoiderView.Get<ECS::Read<STransformComponent>>(State.Entities[i]).GetWorldLocationCached() + GDebugLift;

            Context.DrawDebugLine(Base,
                Base + FVector3(Agents.PrefVelX[i], 0.0f, Agents.PrefVelZ[i]) * 0.5f,
                GPreferredColor, 1.5f, -1.0f);

            Context.DrawDebugLine(Base, Base + Agent.AvoidanceVelocity * 0.5f, GSolvedColor, 2.0f, -1.0f);
        };

        // Every write lands on the agent's own components, so a crowd scatters in parallel; debug lines stay serial.
        const auto Scatter = [&](int32 i, bool bAllowDebug) -> int32
        {
            const ECS::FEntity  Entity = State.Entities[i];
            SRVOAgentComponent& Agent  = AvoiderView.Get<ECS::Write<SRVOAgentComponent>>(Entity);

            int32 SolvedHere = 0;
            if ((Agents.Flags[i] & Avoidance::CrowdFlag::Solve) != 0)
            {
                Agent.AvoidanceVelocity = FVector3(Agents.OutVelX[i], 0.0f, Agents.OutVelZ[i]);
                Agent.LastNeighborCount = Agents.OutNeighbors[i];
                SolvedHere = 1;
            }

            if (Agent.bDriveCharacterController)
            {
                if (SCharacterControllerComponent* Controller = Controllers.TryGet<SCharacterControllerComponent>(Entity))
                {
                    const float MaxSpeed = Agents.MaxSpeed[i];
                    if (MaxSpeed > 1e-4f)
                    {
                        FVector3    Input     = Agent.AvoidanceVelocity / MaxSpeed;
                        const float Magnitude = Math::Length(Input);
                        if (Magnitude > 1.0f)
                        {
                            Input /= Magnitude;
                        }
                        Controller->AddWorldMovementInput(Input);
                    }
                }
            }

            if (bAllowDebug && (Agent.bDrawDebug || bForceDebug))
            {
                DrawAgentDebug(i, Agent);
            }
            return SolvedHere;
        };

        // A per-agent debug flag is only known by looking, so the forced CVar is the one case that stays serial up front.
        if (bForceDebug || Count < Avoidance::kCrowdParallelThreshold || !Jobs::IsRuntimeRunning())
        {
            int32 Solved = 0;
            for (int32 i = 0; i < Count; ++i)
            {
                Solved += Scatter(i, true);
            }
            SolvedTotal.store(Solved, std::memory_order_relaxed);
        }
        else
        {
            std::atomic<bool> bAnyDebug{false};
            Jobs::ParallelFor((uint32)Count, [&](const Jobs::FParallelRange& Range)
            {
                int32 Solved = 0;
                bool  bDebug = false;
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    Solved += Scatter((int32)i, false);
                    bDebug |= AvoiderView.Get<ECS::Read<SRVOAgentComponent>>(State.Entities[i]).bDrawDebug;
                }
                SolvedTotal.fetch_add(Solved, std::memory_order_relaxed);
                if (bDebug)
                {
                    bAnyDebug.store(true, std::memory_order_relaxed);
                }
            }, kExtractGrain);

            if (bAnyDebug.load(std::memory_order_relaxed))
            {
                for (int32 i = 0; i < Count; ++i)
                {
                    const SRVOAgentComponent& Agent = AvoiderView.Get<ECS::Read<SRVOAgentComponent>>(State.Entities[i]);
                    if (Agent.bDrawDebug)
                    {
                        DrawAgentDebug(i, Agent);
                    }
                }
            }
        }

        State.LastAgentCount  = Count;
        State.LastSolvedCount = SolvedTotal.load(std::memory_order_relaxed);
    }
}
