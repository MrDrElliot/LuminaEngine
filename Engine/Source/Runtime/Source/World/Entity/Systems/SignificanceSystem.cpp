#include "RuntimePCH.h"
#include "SignificanceSystem.h"
#include "World/ECS/Registry.h"

#include "Core/Console/ConsoleVariable.h"
#include "Core/Math/Frustum.h"
#include "SystemSingletons.h"
#include "TaskSystem/TaskSystem.h"
#include "World/World.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    static TConsoleVar<bool> CVarSignificanceEnabled("Significance.Enabled", true,
        "Score entity significance each frame. Off makes every lookup report full significance.");

    FSystemAccess SSignificanceSystem::Access = FSystemAccess{}
        .Write<SystemResource::Significance>()
        .Read<STransformComponent, SStaticMeshComponent, SSkeletalMeshComponent, SDynamicMeshComponent>();

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

    void SSignificanceSystem::Startup(const FSystemContext& Context) noexcept
    {
        Context.GetRegistry().Ctx().Emplace<FSignificanceState>();
    }

    void SSignificanceSystem::Update(const FSystemContext& Context) noexcept
    {
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

        auto View = Context.CreateView<STransformComponent>();
        const ECS::FSparseSet* Driver = View.GetDriver();
        if (Driver == nullptr || Driver->IsEmpty())
        {
            return;
        }

        const FVector3 ViewOrigin = Resolved->ViewVolume.GetViewPosition();
        const FFrustum Frustum    = Resolved->ViewVolume.GetFrustum();

        // Linear over the dense array, which beats growing the table from inside the parallel body.
        const ECS::FEntity* Dense = Driver->GetDenseData();
        const size_t   DenseNum = Driver->GetDenseSize();
        uint32 MaxIndex = 0;
        for (size_t i = 0; i < DenseNum; ++i)
        {
            MaxIndex = Math::Max(MaxIndex, Dense[i].GetIndex());
        }

        if ((uint32)State.ByEntityIndex.size() <= MaxIndex)
        {
            State.ByEntityIndex.resize((size_t)MaxIndex + 1u);
        }

        ++State.Stamp;
        State.ViewOrigin = ViewOrigin;
        State.bHasView   = true;

        auto StaticStorage   = Context.GetStorage<SStaticMeshComponent>();
        auto SkeletalStorage = Context.GetStorage<SSkeletalMeshComponent>();
        auto DynamicStorage  = Context.GetStorage<SDynamicMeshComponent>();

        const uint32 Stamp = State.Stamp;
        FEntitySignificance* Scores = State.ByEntityIndex.data();

        // Every entity owns a distinct index, so the parallel body never writes the same slot twice.
        const auto Score = [&](ECS::FEntity Entity)
        {
            const STransformComponent& Xform = View.Get<STransformComponent>(Entity);
            const VTransform World = Xform.GetWorldTransformCached();
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

        if (DenseNum < kSignificanceParallelGrain || GTaskSystem == nullptr)
        {
            for (ECS::FEntity Entity : View)
            {
                Score(Entity);
            }
            return;
        }

        Task::ParallelFor((uint32)View.NumDenseSlots(), [&](const Task::FParallelRange& Range)
        {
            View.ForEachInRange(Range.Start, Range.End, [&](ECS::FEntity Entity, STransformComponent&)
            {
                Score(Entity);
            });
        }, 256);
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
