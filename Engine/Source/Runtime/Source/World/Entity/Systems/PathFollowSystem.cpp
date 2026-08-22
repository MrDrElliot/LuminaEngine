#include "RuntimePCH.h"
#include "PathFollowSystem.h"

#include "AI/Navigation/NavMesh.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/Entity/Components/PathFollowComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/NavMeshSystem.h"

namespace Lumina
{
    // STransformComponent is READ-only here, so this batches in parallel with other readers.
    FSystemAccess SPathFollowSystem::Access = FSystemAccess{}
        .Write<SPathFollowComponent, SCharacterControllerComponent>()
        .Read<STransformComponent, FRelationshipComponent, SNavMeshComponent>();

    namespace
    {
        void StorePath(SPathFollowComponent& Comp, const FNavPath& Path)
        {
            const int32 N = (int32)std::min<size_t>(Path.Corners.size(), (size_t)SPathFollowComponent::MaxCorners);
            for (int32 i = 0; i < N; ++i)
            {
                Comp.PathCorners[i] = Path.Corners[i];
            }
            Comp.CornerCount = N;
            Comp.CurrentCorner = 0;
        }

        // Caller must flush dirty transforms before calling from a parallel body.
        bool ResolveGoal(const FSystemContext& Context, SPathFollowComponent& Comp, FVector3& OutGoal)
        {
            if (Comp.TargetEntity != entt::null)
            {
                if (!Context.IsValidEntity(Comp.TargetEntity))
                {
                    Comp.bHasTarget = false;
                    Comp.TargetEntity = entt::null;
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

    void SPathFollowSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();
        
        constexpr FVector3 Lift(0.0f, 0.1f, 0.0f);
        constexpr FVector4 PathColor(0.4f, 0.8f, 1.0f, 1.0f);
        constexpr FVector4 ActiveSegmentColor(1.0f, 0.95f, 0.2f, 1.0f);
        constexpr FVector4 GoalColor(1.0f, 0.4f, 0.4f, 1.0f);

        const float DeltaTime = (float)Context.GetDeltaTime();
        auto View = Context.CreateView<SPathFollowComponent>();
        auto Handle = View.handle();
        if (Handle->empty())
        {
            return;
        }
        
        FNavMesh* const NavMesh = Nav::GetReadyNavMesh(Context);

        auto&& TransformStorage = Context.GetRegistry().storage<STransformComponent>();
        Task::ParallelFor((uint32)Handle->size(), [&](uint32 Index)
        {
            entt::entity Entity = (*Handle)[Index];
            if (!View.contains(Entity))
            {
                return;
            }
            
            SPathFollowComponent& Comp  = View.get<SPathFollowComponent>(Entity);
            STransformComponent&  Xform = TransformStorage.get(Entity);

            Comp.TimeSinceLastPath += DeltaTime;

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
            const bool bIntervalElapsed = Comp.TimeSinceLastPath > Comp.RepathInterval;
            // No CornerCount==0 trigger, or an unreachable goal would re-query every tick.
            const bool bNeedRepath = Comp.bPathDirty || bMovedTarget || bIntervalElapsed;

            if (bNeedRepath)
            {
                FNavPath Path;
                FNavQueryFilter Filter;
                if (NavMesh && NavMesh->FindPath(AgentPos, Goal, Filter, Path) && Path.bValid)
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
                if (Comp.Status == EPathFollowStatus::Following)
                {
                    Comp.Status = EPathFollowStatus::Reached;
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
                if (auto* CC = Context.TryGet<SCharacterControllerComponent>(Entity))
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
    }
}
