#include "RuntimePCH.h"
#include "PathFollowSystem.h"
#include "World/ECS/Registry.h"

#include "AI/Navigation/NavMesh.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/Entity/Components/PathFollowComponent.h"
#include "World/Entity/Components/RVOAgentComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/VehicleComponent.h"
#include "World/Entity/Systems/NavMeshSystem.h"
#include "World/Entity/Systems/SignificanceSystem.h"

namespace Lumina
{
    // STransformComponent is READ-only here, so this batches in parallel with other readers.
    void SPathFollowSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        Writes<SPathFollowComponent, SCharacterControllerComponent, SRVOAgentComponent, SVehicleComponent>();
        Reads<STransformComponent, SNavMeshComponent,
              SCharacterMovementComponent, SystemResource::Significance>();
    }

    namespace
    {
        void StorePath(SPathFollowComponent& Comp, const FNavPath& Path, const FVector3& AgentPos)
        {
            const int32 N = (int32)std::min<size_t>(Path.Corners.size(), (size_t)SPathFollowComponent::MaxCorners);
            for (int32 i = 0; i < N; ++i)
            {
                Comp.PathCorners[i] = Path.Corners[i];
                Comp.PathCornerFlags[i] = i < (int32)Path.CornerFlags.size() ? Path.CornerFlags[i] : 0;
            }
            Comp.CornerCount = N;
            Comp.CurrentCorner = 0;
            Comp.PathEpoch = Path.Epoch;
            Comp.bPathPartial = Path.bPartial;
            Comp.bPathTruncated = Path.bTruncated || (int32)Path.Corners.size() > N;

            // A search that ran out of nodes before leaving the agent's feet is an unreachable goal, and repathing from there would loop forever.
            if (Comp.bPathTruncated && N > 0)
            {
                const FVector3 Last = Comp.PathCorners[N - 1] - AgentPos;
                if (Math::Length(FVector3(Last.x, 0.0f, Last.z)) <= Comp.AcceptanceRadius)
                {
                    Comp.bPathTruncated = false;
                    Comp.bPathPartial = true;
                }
            }
        }

        // Caller must flush dirty transforms before calling from a parallel body.
        bool ResolveGoal(const FSystemContext& Context, SPathFollowComponent& Comp, FVector3& OutGoal)
        {
            if (Comp.TargetEntity != ECS::NullEntity)
            {
                if (!Context.IsValidEntity(Comp.TargetEntity))
                {
                    Comp.bHasTarget = false;
                    Comp.TargetEntity = ECS::NullEntity;
                    return false;
                }
                if (auto* T = Context.TryGet<STransformComponent>(Comp.TargetEntity))
                {
                    Comp.TargetLocation = T->GetWorldLocationCached();
                }
            }
            OutGoal = Comp.TargetLocation;
            return true;
        }

        struct FAgentMesh
        {
            FName       Agent;
            FNavMesh*   Mesh = nullptr;
        };

        // Resolved once per tick from a handful of volumes, since followers query it from a parallel loop.
        FNavMesh* MeshForAgent(const TVector<FAgentMesh>& Meshes, FName Agent)
        {
            FNavMesh* Fallback = nullptr;
            for (const FAgentMesh& Entry : Meshes)
            {
                if (Entry.Agent == Agent)
                {
                    return Entry.Mesh;
                }
                Fallback = Fallback != nullptr ? Fallback : Entry.Mesh;
            }
            return Agent.IsNone() ? Fallback : nullptr;
        }

        void ReleaseVehicle(SPathFollowComponent& Comp, SVehicleComponent* Vehicle)
        {
            if (Comp.bDrivingVehicle && Vehicle != nullptr)
            {
                Vehicle->SetInput(0.0f, 0.0f);
            }
            Comp.bDrivingVehicle = false;
            Comp.VehicleStuckTime = 0.0f;
            Comp.VehicleReverseTime = 0.0f;
        }

        // Pure pursuit on the next corner, backing out when the vehicle has sat against something with the throttle open.
        void DriveVehicle(SPathFollowComponent& Comp, SVehicleComponent& Vehicle, const FVector3& Heading, const FVector3& Direction,
                          float GoalDistance, float DeltaTime)
        {
            const FVector3 Forward = Math::Normalize(FVector3(Heading.x, 0.0f, Heading.z));
            const FVector3 Right(Forward.z, 0.0f, -Forward.x);
            const float Angle = Math::Atan2(Math::Dot(Direction, Right), Math::Dot(Direction, Forward));
            const bool bSkid = Vehicle.Steering == EVehicleSteering::Skid;

            const float SteerRange = Math::Radians(bSkid ? 45.0f : Math::Max(Vehicle.MaxSteerAngle, 1.0f));
            float Steer = Math::Clamp(Angle / SteerRange, -1.0f, 1.0f);
            float Throttle = Comp.Speed * Math::Lerp(1.0f, 0.6f, Math::Abs(Steer));

            // A low cruise throttle cannot start a heavy vehicle up a slope or round a tight turn from rest.
            constexpr float PullAwayThrottle = 0.45f;
            if (Math::Abs(Vehicle.ForwardSpeed) < 2.0f && Comp.Speed > 0.0f)
            {
                Throttle = Math::Max(Throttle, PullAwayThrottle);
            }

            if (bSkid && Math::Abs(Angle) > Math::Radians(60.0f))
            {
                Throttle = 0.0f;
            }
            else if (!bSkid && Math::Abs(Angle) > Math::Radians(110.0f) && GoalDistance < 15.0f)
            {
                // A goal just behind a car is closer reversed than driven around.
                Throttle = -Comp.Speed * 0.6f;
                Steer = -Math::Sign(Angle);
            }

            if (Comp.VehicleBrakingDistance > 0.0f && GoalDistance < Comp.VehicleBrakingDistance)
            {
                Throttle *= Math::Max(GoalDistance / Comp.VehicleBrakingDistance, 0.25f);
            }

            if (Comp.VehicleReverseTime > 0.0f)
            {
                Comp.VehicleReverseTime -= DeltaTime;
                Throttle = -0.7f;
                Steer = -Steer;
            }
            else if (Throttle > 0.05f && Math::Abs(Vehicle.ForwardSpeed) < 0.5f)
            {
                Comp.VehicleStuckTime += DeltaTime;
                if (Comp.VehicleStuckTime > 1.5f)
                {
                    Comp.VehicleStuckTime = 0.0f;
                    Comp.VehicleReverseTime = 1.2f;
                }
            }
            else
            {
                Comp.VehicleStuckTime = 0.0f;
            }

            Vehicle.SetInput(Throttle, Steer);
            Comp.bDrivingVehicle = true;
        }
    }

    void SPathFollowSystem::OnUpdate()
    {
        const FSystemContext& Context = GetContext();

        LUMINA_PROFILE_SCOPE();
        
        constexpr FVector3 Lift(0.0f, 0.1f, 0.0f);
        constexpr FVector4 PathColor(0.4f, 0.8f, 1.0f, 1.0f);
        constexpr FVector4 ActiveSegmentColor(1.0f, 0.95f, 0.2f, 1.0f);
        constexpr FVector4 GoalColor(1.0f, 0.4f, 0.4f, 1.0f);

        const float DeltaTime = (float)Context.GetDeltaTime();
        auto View = Context.CreateView<SPathFollowComponent>();
        auto Handle = View.GetDriver();
        if (Handle->IsEmpty())
        {
            return;
        }
        
        TVector<FAgentMesh> AgentMeshes;
        Context.CreateView<SNavMeshComponent>().ForEach([&](const SNavMeshComponent& Volume)
        {
            if (Volume.Runtime.Mesh && Volume.Runtime.Mesh->IsReady())
            {
                AgentMeshes.push_back({ Volume.Agent, Volume.Runtime.Mesh.get() });
            }
        });

        auto TransformStorage = Context.GetRegistry().GetStorage<STransformComponent>();
        auto AvoidanceStorage = Context.GetRegistry().GetStorage<SRVOAgentComponent>();
        auto MovementStorage  = Context.GetRegistry().GetStorage<SCharacterMovementComponent>();
        auto VehicleStorage   = Context.GetRegistry().GetStorage<SVehicleComponent>();
        const FSignificanceState* SignificanceState = Significance::GetState(Context);
        auto PathStorage = Context.GetRegistry().GetStorage<SPathFollowComponent>();

        // Everything up to the repath decision. False when the follower is already settled for this tick.
        const auto Prepare = [&](ECS::FEntity Entity, SPathFollowComponent& Comp, FVector3& OutGoal, FVector3& OutAgentPos, bool& bOutNeedRepath) -> bool
        {
            STransformComponent&  Xform = TransformStorage.Get(Entity);
            SVehicleComponent* Vehicle = VehicleStorage.TryGet(Entity);
            FNavMesh* const NavMesh = MeshForAgent(AgentMeshes, Comp.NavAgent);

            Comp.TimeSinceLastPath += DeltaTime;

            // Cleared up front so every early return below leaves the agent stopped, not coasting.
            SRVOAgentComponent* Avoidance = AvoidanceStorage.TryGet(Entity);
            if (Avoidance != nullptr)
            {
                Avoidance->PreferredVelocity = FVector3(0.0f);
            }

            if (!Comp.bHasTarget)
            {
                Comp.CornerCount = 0;
                Comp.Status = EPathFollowStatus::None;
                ReleaseVehicle(Comp, Vehicle);
                return false;
            }

            FVector3 Goal;
            if (!ResolveGoal(Context, Comp, Goal))
            {
                Comp.CornerCount = 0;
                Comp.Status = EPathFollowStatus::None;
                ReleaseVehicle(Comp, Vehicle);
                return false;
            }

            const FVector3 AgentPos = Xform.GetWorldLocationCached();

            const bool bMovedTarget = Math::Length(Goal - Comp.PathSourceTarget) > Comp.RepathDistance;
            const float RepathInterval = Significance::ScaleInterval(SignificanceState, Entity, Comp.RepathInterval);
            const bool bIntervalElapsed = Comp.TimeSinceLastPath > RepathInterval;

            // The epoch is mesh-wide, so one streamed tile would otherwise repath every agent alive.
            bool bMeshChanged = false;
            if (NavMesh && Comp.CornerCount > 0 && Comp.PathEpoch != NavMesh->GetTopologyEpoch())
            {
                FVector3 CorridorMin = AgentPos;
                FVector3 CorridorMax = AgentPos;
                for (int32 i = Comp.CurrentCorner; i < Comp.CornerCount; ++i)
                {
                    CorridorMin = Math::Min(CorridorMin, Comp.PathCorners[i]);
                    CorridorMax = Math::Max(CorridorMax, Comp.PathCorners[i]);
                }
                const FVector3 Margin(Comp.AcceptanceRadius);
                bMeshChanged = NavMesh->HasTileChangedSince(Comp.PathEpoch, CorridorMin - Margin, CorridorMax + Margin);
                if (!bMeshChanged)
                {
                    // Banked so the same untouched corridor is not re-tested against every later change.
                    Comp.PathEpoch = NavMesh->GetTopologyEpoch();
                }
            }
            // No CornerCount==0 trigger, or an unreachable goal would re-query every tick.
            OutGoal = Goal;
            OutAgentPos = AgentPos;
            bOutNeedRepath = Comp.bPathDirty || bMovedTarget || bIntervalElapsed || bMeshChanged;
            return true;
        };

        // The path query when one is owed, then the corner walk and the steering it produces.
        const auto Follow = [&](ECS::FEntity Entity, SPathFollowComponent& Comp, const FVector3& Goal, const FVector3& AgentPos, bool bNeedRepath)
        {
            STransformComponent&  Xform = TransformStorage.Get(Entity);
            SVehicleComponent* Vehicle = VehicleStorage.TryGet(Entity);
            FNavMesh* const NavMesh = MeshForAgent(AgentMeshes, Comp.NavAgent);
            SRVOAgentComponent* Avoidance = AvoidanceStorage.TryGet(Entity);

            if (bNeedRepath)
            {
                FNavPath Path;
                FNavQueryFilter Filter;
                Filter.MaxCorners = SPathFollowComponent::MaxCorners;
                const bool bFound = NavMesh && NavMesh->FindPath(AgentPos, Goal, Filter, Path) && Path.bValid;
                Comp.LastPathResult = NavMesh ? Path.Result : ENavPathResult::NoNavMesh;

                // Nothing was asked of the navmesh, so this is not a route failure.
                if (!bFound && Path.bQueryUnavailable)
                {
                    return;
                }

                if (bFound)
                {
                    StorePath(Comp, Path, AgentPos);
                    Comp.PathSourceTarget = Goal;
                    Comp.bPathDirty = false;
                    Comp.TimeSinceLastPath = 0.0f;
                    Comp.Status = EPathFollowStatus::Following;
                    Comp.ConsecutiveFailures = 0;
                }
                else
                {
                    // Keep prior corners on failure; reset timer to avoid hammering FindPath every tick.
                    Comp.Status = EPathFollowStatus::Failed;
                    ++Comp.ConsecutiveFailures;
                    Comp.TimeSinceLastPath = 0.0f;
                    Comp.bPathDirty = false;
                    // Banked even though the query failed, or a mesh that keeps changing retries every tick.
                    if (NavMesh)
                    {
                        Comp.PathEpoch = NavMesh->GetTopologyEpoch();
                    }
                    if (Comp.CornerCount == 0)
                    {
                        ReleaseVehicle(Comp, Vehicle);
                        return;
                    }
                }
            }

            while (Comp.CurrentCorner < Comp.CornerCount)
            {
                const FVector3 ToCorner = Comp.PathCorners[Comp.CurrentCorner] - AgentPos;
                const FVector3 Flat(ToCorner.x, 0.0f, ToCorner.z);
                if (Math::Length(Flat) <= Comp.AcceptanceRadius)
                {
                    ++Comp.CurrentCorner;
                    continue;
                }
                break;
            }

            if (Comp.CurrentCorner >= Comp.CornerCount)
            {
                // The route continued past the buffer, so repath rather than call a cut corner the destination.
                if (Comp.bPathTruncated)
                {
                    Comp.bPathDirty = true;
                    return;
                }
                if (Comp.Status == EPathFollowStatus::Following)
                {
                    Comp.Status = Comp.bPathPartial ? EPathFollowStatus::Failed : EPathFollowStatus::Reached;
                }
                ReleaseVehicle(Comp, Vehicle);
                return;
            }

            const FVector3 Dir = Comp.PathCorners[Comp.CurrentCorner] - AgentPos;
            const FVector3 Flat(Dir.x, 0.0f, Dir.z);
            const float Len = Math::Length(Flat);
            if (Len < 1e-4f)
            {
                return;
            }
            FVector3 Move = (Flat / Len) * Comp.Speed;

            // Judged on real displacement, since a walker pinned by something the navmesh cannot see still wants full speed.
            const SCharacterMovementComponent* Walker = Vehicle == nullptr ? MovementStorage.TryGet(Entity) : nullptr;
            if (Walker != nullptr && Comp.StuckTimeout > 0.0f && Comp.Speed > 0.05f)
            {
                if (Comp.SidestepTime > 0.0f)
                {
                    Comp.SidestepTime -= DeltaTime;
                    const float Side = (Comp.StuckAttempts % 2 == 0) ? 1.0f : -1.0f;
                    const FVector3 Across(Flat.z / Len * Side, 0.0f, -Flat.x / Len * Side);
                    Move = Math::Normalize(Across + (Flat / Len) * 0.3f) * Comp.Speed;
                }
                else
                {
                    if (Comp.StuckTime <= 0.0f)
                    {
                        Comp.StuckAnchor = AgentPos;
                    }
                    Comp.StuckTime += DeltaTime;
                    if (Comp.StuckTime >= Comp.StuckTimeout)
                    {
                        const FVector3 Moved = AgentPos - Comp.StuckAnchor;
                        const float Progress = Math::Length(FVector3(Moved.x, 0.0f, Moved.z));
                        const float Expected = Walker->MoveSpeed * Comp.Speed * Comp.StuckTime;
                        if (Progress < Expected * 0.2f)
                        {
                            Comp.SidestepTime = 0.6f;
                            Comp.bPathDirty = true;
                            ++Comp.StuckAttempts;
                        }
                        else if (Progress > Expected * 0.5f)
                        {
                            Comp.StuckAttempts = 0;
                        }
                        Comp.StuckTime = 0.0f;
                    }
                }
            }

            if (Comp.bDriveCharacterController)
            {
                if (Vehicle != nullptr)
                {
                    const FVector3 Final = Comp.PathCorners[Comp.CornerCount - 1] - AgentPos;
                    const float GoalDistance = Comp.bPathTruncated ? FLT_MAX : Math::Length(FVector3(Final.x, 0.0f, Final.z));
                    DriveVehicle(Comp, *Vehicle, Xform.GetWorldTransformCached().GetForward(), Flat / Len, GoalDistance, DeltaTime);
                }
                // An avoidance agent consumes the desired velocity instead, and drives the controller itself.
                else if (Avoidance != nullptr)
                {
                    const SCharacterMovementComponent* Movement = MovementStorage.TryGet(Entity);
                    const float Speed = Avoidance->MaxSpeed > 0.0f
                        ? Avoidance->MaxSpeed
                        : (Movement != nullptr ? Movement->MoveSpeed : 1.0f);

                    Avoidance->PreferredVelocity = Move * Speed;
                }
                else if (auto* CC = Context.TryGet<SCharacterControllerComponent>(Entity))
                {
                    CC->AddWorldMovementInput(Move);
                }
            }

            if (!Comp.bDrawDebugPath || Comp.CornerCount == 0)
            {
                return;
            }
            
            // Active segment from the agent to the next pending corner.
            const int32 Cur = Math::Min(Comp.CurrentCorner, Comp.CornerCount - 1);
            Context.DrawDebugLine(AgentPos, Comp.PathCorners[Cur] + Lift, ActiveSegmentColor, 2.0f, -1.0f);

            // Remaining corner-to-corner segments.
            for (int32 i = Cur; i + 1 < Comp.CornerCount; ++i)
            {
                Context.DrawDebugLine(Comp.PathCorners[i] + Lift, Comp.PathCorners[i + 1] + Lift, PathColor, 1.5f, -1.0f);
            }

            const float CrossSize = Math::Max(Comp.AcceptanceRadius, 0.25f);
            Context.DrawDebugLine(Goal - FVector3(CrossSize, 0, 0), Goal + FVector3(CrossSize, 0, 0), GoalColor, 1.5f, -1.0f);
            Context.DrawDebugLine(Goal - FVector3(0, 0, CrossSize), Goal + FVector3(0, 0, CrossSize), GoalColor, 1.5f, -1.0f);
        };

        const uint32 ThreadSlots = Math::Max(GTaskSystem->GetNumTaskThreads(), 1u);
        if ((uint32)RepathBuckets.size() < ThreadSlots)
        {
            RepathBuckets.resize(ThreadSlots);
        }
        for (FRepathBucket& Bucket : RepathBuckets)
        {
            Bucket.Requests.clear();
        }

        // A path query costs as much as hundreds of plain followers, so queries are set aside rather than run inside a chunk.
        Task::ParallelFor((uint32)View.NumDenseSlots(), [&](const Task::FParallelRange& Range)
        {
            View.ForEachInRange(Range.Start, Range.End, [&](ECS::FEntity Entity, SPathFollowComponent& Comp)
            {
                FVector3 Goal;
                FVector3 AgentPos;
                bool bNeedRepath = false;
                if (!Prepare(Entity, Comp, Goal, AgentPos, bNeedRepath))
                {
                    return;
                }
                if (bNeedRepath)
                {
                    RepathBuckets[Range.Thread].Requests.push_back({ Entity, Goal, AgentPos });
                    return;
                }
                Follow(Entity, Comp, Goal, AgentPos, false);
            });
        }, 64);

        RepathQueue.clear();
        for (const FRepathBucket& Bucket : RepathBuckets)
        {
            RepathQueue.insert(RepathQueue.end(), Bucket.Requests.begin(), Bucket.Requests.end());
        }

        // One query per grab, so a burst of retargets spreads across every worker instead of piling onto a few chunks.
        Task::ParallelFor((uint32)RepathQueue.size(), [&](const Task::FParallelRange& Range)
        {
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                const FRepathRequest& Request = RepathQueue[i];
                Follow(Request.Entity, PathStorage.Get(Request.Entity), Request.Goal, Request.AgentPos, true);
            }
        }, 1);
    }
}
