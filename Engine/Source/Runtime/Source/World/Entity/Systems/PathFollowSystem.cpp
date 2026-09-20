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
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/NavMeshSystem.h"
#include "World/Entity/Systems/SignificanceSystem.h"

namespace Lumina
{
    // STransformComponent is READ-only here, so this batches in parallel with other readers.
    void SPathFollowSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        Writes<SPathFollowComponent, SCharacterControllerComponent, SRVOAgentComponent>();
        Reads<STransformComponent, FRelationshipComponent, SNavMeshComponent,
              SCharacterMovementComponent, SystemResource::Significance>();
    }

    namespace
    {
        void StorePath(SPathFollowComponent& Comp, const FNavPath& Path)
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
        
        FNavMesh* const NavMesh = Nav::GetReadyNavMesh(Context);

        auto TransformStorage = Context.GetRegistry().GetStorage<STransformComponent>();
        auto AvoidanceStorage = Context.GetRegistry().GetStorage<SRVOAgentComponent>();
        auto MovementStorage  = Context.GetRegistry().GetStorage<SCharacterMovementComponent>();
        const FSignificanceState* SignificanceState = Significance::GetState(Context);
        // Chunked, so the scheduler is not asked to dispatch one job per follower.
        Task::ParallelFor((uint32)View.NumDenseSlots(), [&](const Task::FParallelRange& Range)
        {
            View.ForEachInRange(Range.Start, Range.End, [&](ECS::FEntity Entity, SPathFollowComponent& Comp)
            {
                STransformComponent&  Xform = TransformStorage.Get(Entity);

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
                    return;
                }

                FVector3 Goal;
                if (!ResolveGoal(Context, Comp, Goal))
                {
                    Comp.CornerCount = 0;
                    Comp.Status = EPathFollowStatus::None;
                    return;
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
                const bool bNeedRepath = Comp.bPathDirty || bMovedTarget || bIntervalElapsed || bMeshChanged;

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
                        StorePath(Comp, Path);
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
                    return;
                }

                const FVector3 Dir = Comp.PathCorners[Comp.CurrentCorner] - AgentPos;
                const FVector3 Flat(Dir.x, 0.0f, Dir.z);
                const float Len = Math::Length(Flat);
                if (Len < 1e-4f)
                {
                    return;
                }
                const FVector3 Move = (Flat / Len) * Comp.Speed;

                if (Comp.bDriveCharacterController)
                {
                    // An avoidance agent consumes the desired velocity instead, and drives the controller itself.
                    if (Avoidance != nullptr)
                    {
                        const SCharacterMovementComponent* Movement = MovementStorage.TryGet(Entity);
                        const float Speed = Avoidance->MaxSpeed > 0.0f
                            ? Avoidance->MaxSpeed
                            : (Movement != nullptr ? Movement->MoveSpeed : 1.0f);

                        Avoidance->PreferredVelocity = Move * Speed;
                    }
                    else if (auto* CC = Context.TryGet<SCharacterControllerComponent>(Entity))
                    {
                        CC->AddMovementInput(Move);
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
            });
        }, 64);
    }
}
