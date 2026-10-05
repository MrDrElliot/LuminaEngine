#include "RuntimePCH.h"
#include "RVOAvoidanceSystem.h"
#include "World/ECS/Registry.h"

#include "AI/Avoidance/ORCA.h"
#include "AI/Navigation/NavMesh.h"
#include "Core/Console/ConsoleVariable.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/Entity/Components/RVOAgentComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/NavMeshSystem.h"
#include "World/Entity/Systems/SignificanceSystem.h"

namespace Lumina
{
    static TConsoleVar<bool> CVarRVOEnabled("AI.RVO.Enabled", true,
        "Solve reciprocal local avoidance. Off passes the preferred velocity through untouched.");

    static TConsoleVar<bool> CVarRVODrawDebug("AI.RVO.DrawDebug", false,
        "Draw preferred and solved velocities for every avoidance agent, ignoring the per-agent flag.");

    namespace
    {
        constexpr uint32 kExtractGrain = 256;

        constexpr FVector4 GPreferredColor(0.4f, 0.8f, 1.0f, 1.0f);
        constexpr FVector4 GSolvedColor(1.0f, 0.75f, 0.2f, 1.0f);
        constexpr FVector3 GDebugLift(0.0f, 0.15f, 0.0f);
    }

    void SRVOAvoidanceSystem::Configure()
    {
        // Low sorts after SPathFollowSystem's Default, so the preferred velocity is already written.
        RequireUpdate(EUpdateStage::PrePhysics, EUpdatePriority::Low);
        Writes<SRVOAgentComponent, SCharacterControllerComponent>();
        Reads<STransformComponent, SCharacterPhysicsComponent, SCharacterMovementComponent,
              SNavMeshComponent, SystemResource::Significance>();
    }

    void SRVOAvoidanceSystem::OnStartup()
    {
        GetContext().GetRegistry().Ctx().Emplace<FAvoidanceState>();
    }

    void SRVOAvoidanceSystem::OnUpdate()
    {
        const FSystemContext& Context = GetContext();

        LUMINA_PROFILE_SCOPE();

        FAvoidanceState* StatePtr = Context.GetRegistry().Ctx().Find<FAvoidanceState>();
        if (StatePtr == nullptr)
        {
            return;
        }

        FAvoidanceState& State = *StatePtr;
        State.bEnabled = CVarRVOEnabled.GetValue();
        State.LastAgentCount = 0;
        State.LastSolvedCount = 0;
        State.LastNavClamped = 0;

        auto AgentStorage = Context.GetRegistry().GetStorage<SRVOAgentComponent>();
        const size_t DenseSize = AgentStorage.GetDenseSize();
        if (DenseSize == 0)
        {
            return;
        }

        const float DeltaTime   = (float)Context.GetDeltaTime();
        const float InvTimeStep = DeltaTime > 1e-4f ? 1.0f / DeltaTime : 60.0f;
        const bool  bForceDebug = CVarRVODrawDebug.GetValue();

        Avoidance::FCrowdAgents& Agents = State.Agents;
        Agents.Reset(DenseSize);
        State.Entities.resize(DenseSize);

        auto TransformStorage = Context.GetRegistry().GetStorage<STransformComponent>();
        auto CharacterStorage = Context.GetRegistry().GetStorage<SCharacterPhysicsComponent>();
        auto MovementStorage  = Context.GetRegistry().GetStorage<SCharacterMovementComponent>();
        const FSignificanceState* SignificanceState = Significance::GetState(Context);

        ++State.FrameCounter;
        const uint32 Frame = State.FrameCounter;

        const ECS::FEntity* Dense = AgentStorage.GetDenseData();

        // Compacted first, because the dense array carries tombstones and the solve indexes by position.
        int32 Count = 0;
        for (size_t i = 0; i < DenseSize; ++i)
        {
            const ECS::FEntity Entity = Dense[i];
            if (Entity.IsTombstone() || !TransformStorage.Contains(Entity))
            {
                continue;
            }
            State.Entities[(size_t)Count++] = Entity;
        }

        if (Count == 0)
        {
            return;
        }

        const bool bSolveEnabled = State.bEnabled;

        const auto Extract = [&](int32 i)
        {
            const ECS::FEntity  Entity = State.Entities[(size_t)i];
            SRVOAgentComponent& Agent  = AgentStorage.Get(Entity);

            const FVector3 Position = TransformStorage.Get(Entity).GetWorldLocationCached();

            float CharacterRadius = 0.5f;
            if (const SCharacterPhysicsComponent* Character = CharacterStorage.TryGet(Entity))
            {
                CharacterRadius = Character->Radius;
            }

            float    Speed = 5.0f;
            FVector3 Velocity(0.0f);
            if (const SCharacterMovementComponent* Movement = MovementStorage.TryGet(Entity))
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

            const uint8 Interval = SignificanceState != nullptr
                ? Math::Max(SignificanceState->Get(Entity).TickInterval, (uint8)1)
                : (uint8)1;

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

            Agents.PosX[(size_t)i]           = Position.x;
            Agents.PosZ[(size_t)i]           = Position.z;
            Agents.VelX[(size_t)i]           = Velocity.x;
            Agents.VelZ[(size_t)i]           = Velocity.z;
            Agents.PrefVelX[(size_t)i]       = PrefX;
            Agents.PrefVelZ[(size_t)i]       = PrefZ;
            Agents.Radius[(size_t)i]         = Radius;
            Agents.MaxSpeed[(size_t)i]       = MaxSpeed;
            Agents.InvTimeHorizon[(size_t)i] = 1.0f / Horizon;
            Agents.NeighborRange[(size_t)i]  = Range;
            Agents.Responsibility[(size_t)i] = Math::Clamp(Agent.Responsibility, 0.0f, 1.0f);
            Agents.MaxNeighbors[(size_t)i]   = Neighbors;
            Agents.Flags[(size_t)i]          = Flags;

            // Held from the previous solve, so an agent skipping this frame keeps a sane output.
            Agents.OutVelX[(size_t)i]      = Agent.AvoidanceVelocity.x;
            Agents.OutVelZ[(size_t)i]      = Agent.AvoidanceVelocity.z;
            Agents.OutNeighbors[(size_t)i] = Agent.LastNeighborCount;
        };

        if (Count < Avoidance::kCrowdParallelThreshold || GTaskSystem == nullptr)
        {
            for (int32 i = 0; i < Count; ++i)
            {
                Extract(i);
            }
        }
        else
        {
            Task::ParallelFor((uint32)Count, [&](const Task::FParallelRange& Range)
            {
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    Extract((int32)i);
                }
            }, kExtractGrain);
        }

        Avoidance::SolveCrowd(Agents, Count, InvTimeStep, true);

        // Gathered rather than tested inline, so the raycast pass is empty unless someone opted in.
        State.NavClampList.clear();
        FNavMesh* const NavMesh = Nav::GetReadyNavMesh(Context);
        if (NavMesh != nullptr)
        {
            for (int32 i = 0; i < Count; ++i)
            {
                if ((Agents.Flags[(size_t)i] & Avoidance::CrowdFlag::NavClamp) != 0)
                {
                    State.NavClampList.push_back(i);
                }
            }
        }

        if (!State.NavClampList.empty())
        {
            const float Lookahead = DeltaTime > 1e-4f ? DeltaTime : (1.0f / 60.0f);

            const auto Clamp = [&](int32 Slot)
            {
                const int32 i = State.NavClampList[(size_t)Slot];

                const float SolvedX = Agents.OutVelX[(size_t)i];
                const float SolvedZ = Agents.OutVelZ[(size_t)i];
                if (SolvedX == 0.0f && SolvedZ == 0.0f)
                {
                    return;
                }

                const FVector3 From(Agents.PosX[(size_t)i], 0.0f, Agents.PosZ[(size_t)i]);
                const FVector3 To(From.x + SolvedX * Lookahead, 0.0f, From.z + SolvedZ * Lookahead);

                FNavQueryFilter   Filter;
                FNavRaycastResult Hit;
                if (NavMesh->Raycast(From, To, Filter, Hit) && Hit.bHit)
                {
                    // The preferred velocity came off a navmesh path, so it is the safe thing to fall back to.
                    Agents.OutVelX[(size_t)i] = Agents.PrefVelX[(size_t)i];
                    Agents.OutVelZ[(size_t)i] = Agents.PrefVelZ[(size_t)i];
                }
            };

            const int32 NumClamped = (int32)State.NavClampList.size();
            if (NumClamped < Avoidance::kCrowdParallelThreshold || GTaskSystem == nullptr)
            {
                for (int32 Slot = 0; Slot < NumClamped; ++Slot)
                {
                    Clamp(Slot);
                }
            }
            else
            {
                Task::ParallelFor((uint32)NumClamped, [&](const Task::FParallelRange& Range)
                {
                    for (uint32 Slot = Range.Start; Slot < Range.End; ++Slot)
                    {
                        Clamp((int32)Slot);
                    }
                }, 32);
            }

            State.LastNavClamped = NumClamped;
        }

        auto ControllerStorage = Context.GetRegistry().GetStorage<SCharacterControllerComponent>();

        std::atomic<int32> SolvedTotal{0};

        const auto DrawAgentDebug = [&](int32 i, const SRVOAgentComponent& Agent)
        {
            const FVector3 Base = TransformStorage.Get(State.Entities[(size_t)i]).GetWorldLocationCached() + GDebugLift;

            Context.DrawDebugLine(Base,
                Base + FVector3(Agents.PrefVelX[(size_t)i], 0.0f, Agents.PrefVelZ[(size_t)i]) * 0.5f,
                GPreferredColor, 1.5f, -1.0f);

            Context.DrawDebugLine(Base, Base + Agent.AvoidanceVelocity * 0.5f, GSolvedColor, 2.0f, -1.0f);
        };

        // Every write lands on the agent's own components, so a crowd scatters in parallel; debug lines stay serial.
        const auto Scatter = [&](int32 i, bool bAllowDebug) -> int32
        {
            const ECS::FEntity  Entity = State.Entities[(size_t)i];
            SRVOAgentComponent& Agent  = AgentStorage.Get(Entity);

            int32 SolvedHere = 0;
            if ((Agents.Flags[(size_t)i] & Avoidance::CrowdFlag::Solve) != 0)
            {
                Agent.AvoidanceVelocity = FVector3(Agents.OutVelX[(size_t)i], 0.0f, Agents.OutVelZ[(size_t)i]);
                Agent.LastNeighborCount = Agents.OutNeighbors[(size_t)i];
                SolvedHere = 1;
            }

            if (Agent.bDriveCharacterController)
            {
                if (SCharacterControllerComponent* Controller = ControllerStorage.TryGet(Entity))
                {
                    const float MaxSpeed = Agents.MaxSpeed[(size_t)i];
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
        if (bForceDebug || Count < Avoidance::kCrowdParallelThreshold || GTaskSystem == nullptr)
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
            Task::ParallelFor((uint32)Count, [&](const Task::FParallelRange& Range)
            {
                int32 Solved = 0;
                bool  bDebug = false;
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    Solved += Scatter((int32)i, false);
                    bDebug |= AgentStorage.Get(State.Entities[i]).bDrawDebug;
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
                    const SRVOAgentComponent& Agent = AgentStorage.Get(State.Entities[(size_t)i]);
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

    namespace Avoidance
    {
        const FAvoidanceState* GetState(const FSystemContext& Context)
        {
            return Context.GetRegistry().Ctx().Find<FAvoidanceState>();
        }
    }
}
