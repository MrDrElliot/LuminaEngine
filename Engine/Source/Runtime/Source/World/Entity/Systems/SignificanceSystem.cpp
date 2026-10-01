#include "RuntimePCH.h"
#include "SignificanceSystem.h"
#include "World/ECS/Registry.h"

#include "Core/Console/ConsoleVariable.h"
#include "Core/Math/Frustum.h"
#include "SystemSingletons.h"
#include "TaskSystem/TaskSystem.h"
#include "World/World.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/PathFollowComponent.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/RVOAgentComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    static TConsoleVar<bool> CVarSignificanceEnabled("Significance.Enabled", true,
        "Score entity significance each frame. Off makes every lookup report full significance.");

    void SSignificanceSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest);
        RequireUpdate(EUpdateStage::Paused, EUpdatePriority::Highest);
        Writes<SystemResource::Significance>();
        Reads<STransformComponent, SStaticMeshComponent, SSkeletalMeshComponent, SDynamicMeshComponent>();
        Reads<SAudioSourceComponent, SPathFollowComponent, SPerceptionComponent, SRVOAgentComponent>();
    }

    namespace
    {
        // Below this a radius cannot divide the distance without producing a meaningless band.
        constexpr float kSignificanceMinRadius = 0.01f;

        constexpr uint32 kSignificanceParallelGrain = 2048;

        FORCEINLINE float WorldRadius(const SMeshComponent* Mesh, const FVector3& WorldScale)
        {
            if (Mesh == nullptr || Mesh->CachedLocalRadius <= 0.0f)
            {
                return Significance::kDefaultRadius;
            }

            const float MaxScale = Math::Max(Math::Abs(WorldScale.x),
                                   Math::Max(Math::Abs(WorldScale.y), Math::Abs(WorldScale.z)));

            return Math::Max(Mesh->CachedLocalRadius * MaxScale * Mesh->BoundsScale, kSignificanceMinRadius);
        }
    }

    void SSignificanceSystem::OnStartup()
    {
        const FSystemContext& Context = GetContext();

        Context.GetRegistry().Ctx().Emplace<FSignificanceState>();
    }

    void SSignificanceSystem::OnUpdate()
    {
        const FSystemContext& Context = GetContext();

        LUMINA_PROFILE_SCOPE();

        FSignificanceState* StatePtr = Context.GetRegistry().Ctx().Find<FSignificanceState>();
        if (StatePtr == nullptr)
        {
            return;
        }

        FSignificanceState& State = *StatePtr;
        State.bEnabled = CVarSignificanceEnabled.GetValue();
        State.bHasView = false;

        if (!State.bEnabled)
        {
            return;
        }

        // One frame old, which is what every broad-phase consumer of this already tolerates.
        const FResolvedSceneView* Resolved = Context.GetRegistry().Ctx().Find<FResolvedSceneView>();
        if (Resolved == nullptr || !Resolved->bHasView)
        {
            return;
        }

        // Only the components whose systems read a score, so static scenery and crowds nobody asks about cost nothing.
        const ECS::FSparseSet* Consumers[] =
        {
            Context.CreateView<SAudioSourceComponent>().GetDriver(),
            Context.CreateView<SPathFollowComponent>().GetDriver(),
            Context.CreateView<SPerceptionComponent>().GetDriver(),
            Context.CreateView<SRVOAgentComponent>().GetDriver(),
        };

        uint32 MaxIndex = 0;
        bool bAnyConsumer = false;
        for (const ECS::FSparseSet* Pool : Consumers)
        {
            if (Pool == nullptr || Pool->IsEmpty())
            {
                continue;
            }
            bAnyConsumer = true;
            const ECS::FEntity* Dense = Pool->GetDenseData();
            for (size_t i = 0, Num = Pool->GetDenseSize(); i < Num; ++i)
            {
                if (!Dense[i].IsTombstone())
                {
                    MaxIndex = Math::Max(MaxIndex, Dense[i].GetIndex());
                }
            }
        }

        const FVector3 ViewOrigin = Resolved->ViewVolume.GetViewPosition();
        const FFrustum Frustum    = Resolved->ViewVolume.GetFrustum();

        ++State.Stamp;
        State.ViewOrigin = ViewOrigin;
        State.bHasView   = true;

        if (!bAnyConsumer)
        {
            return;
        }

        if ((uint32)State.ByEntityIndex.size() <= MaxIndex)
        {
            State.ByEntityIndex.resize((size_t)MaxIndex + 1u);
        }

        auto TransformStorage = Context.GetStorage<STransformComponent>();
        auto StaticStorage    = Context.GetStorage<SStaticMeshComponent>();
        auto SkeletalStorage  = Context.GetStorage<SSkeletalMeshComponent>();
        auto DynamicStorage   = Context.GetStorage<SDynamicMeshComponent>();

        const uint32 Stamp = State.Stamp;
        FEntitySignificance* Scores = State.ByEntityIndex.data();

        // Every entity owns a distinct index, and one in two pools writes the same score twice, so the body never races.
        const auto Score = [&](ECS::FEntity Entity)
        {
            const STransformComponent* Xform = Entity.IsTombstone() ? nullptr : TransformStorage.TryGet(Entity);
            if (Xform == nullptr)
            {
                return;
            }

            const VTransform World = Xform->GetWorldTransformCached();
            const FVector3 Location = World.GetLocation();

            const SMeshComponent* Mesh = StaticStorage.TryGet(Entity);
            if (Mesh == nullptr)
            {
                Mesh = SkeletalStorage.TryGet(Entity);
            }
            if (Mesh == nullptr)
            {
                Mesh = DynamicStorage.TryGet(Entity);
            }

            const float Radius   = WorldRadius(Mesh, World.GetScale());
            const FVector3 ToEye = Location - ViewOrigin;
            const float DistSq   = Math::Dot(ToEye, ToEye);

            FEntitySignificance& Out = Scores[Entity.GetIndex()];
            Out.Owner              = Entity;
            Out.Stamp              = Stamp;
            Out.DistanceSq         = DistSq;
            Out.DistanceOverRadius = Math::Sqrt(DistSq) / Radius;
            Out.TickInterval       = Significance::IntervalForDistanceOverRadius(Out.DistanceOverRadius);
            Out.bInView            = Frustum.IntersectsSphere(Location, Radius);
        };

        for (const ECS::FSparseSet* Pool : Consumers)
        {
            if (Pool == nullptr || Pool->IsEmpty())
            {
                continue;
            }

            const ECS::FEntity* Dense = Pool->GetDenseData();
            const uint32 Num = (uint32)Pool->GetDenseSize();
            if (Num < kSignificanceParallelGrain || GTaskSystem == nullptr)
            {
                for (uint32 i = 0; i < Num; ++i)
                {
                    Score(Dense[i]);
                }
                continue;
            }

            Task::ParallelFor(Num, [&](const Task::FParallelRange& Range)
            {
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    Score(Dense[i]);
                }
            }, 256);
        }
    }

    namespace Significance
    {
        const FSignificanceState* GetState(const FSystemContext& Context)
        {
            return Context.GetRegistry().Ctx().Find<FSignificanceState>();
        }

        const FSignificanceState* GetState(CWorld* World)
        {
            if (World == nullptr)
            {
                return nullptr;
            }
            return ECS::GetWorldRegistry(*World).Ctx().Find<FSignificanceState>();
        }
    }
}
