#include "RuntimePCH.h"
#include "SignificanceSystem.h"
#include "World/ECS/Registry.h"

#include "Core/Console/ConsoleVariable.h"
#include "Core/Math/Frustum.h"
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

    namespace SignificanceSystemInternal
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

    void CSignificanceSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
        RequireUpdate(EUpdateStage::Paused);
    }

    void CSignificanceSystem::OnUpdate(const SSystemContext& Context)
    {
        using namespace SignificanceSystemInternal;

        FSignificanceState& SignificanceState = *State;
        SignificanceState.bEnabled = CVarSignificanceEnabled.GetValue();
        SignificanceState.bHasView = false;

        if (!SignificanceState.bEnabled)
        {
            return;
        }

        // One frame old, which is what every broad-phase consumer of this already tolerates.
        const FResolvedSceneView& Resolved = *ResolvedView;
        if (!Resolved.bHasView)
        {
            return;
        }

        const FVector3 ViewOrigin = Resolved.ViewVolume.GetViewPosition();
        const FFrustum Frustum    = Resolved.ViewVolume.GetFrustum();

        SignificanceState.ViewOrigin = ViewOrigin;
        SignificanceState.bHasView   = true;

        // Only the components whose systems read a score, so static scenery and crowds nobody asks about cost nothing.
        TStampedEntityCache<FEntitySignificance>& Cache = SignificanceState.Scores;
        if (!Cache.BeginPass(AudioSourceConsumers.View(), PathFollowConsumers.View(), PerceptionConsumers.View(),
                             AvoidanceConsumers.View()))
        {
            return;
        }

        const uint32 Stamp = Cache.Stamp;
        FEntitySignificance* Scores = Cache.ByEntityIndex.GetData();

        // Every entity owns a distinct index, and one in two pools writes the same score twice, so the body never races.
        const auto Score = [&](ECS::FEntity Entity, const auto&)
        {
            const STransformComponent* Xform = Transforms.TryGet<STransformComponent>(Entity);
            if (Xform == nullptr)
            {
                return;
            }

            const VTransform World = Xform->GetWorldTransformCached();
            const FVector3 Location = World.GetLocation();

            const SMeshComponent* Mesh = StaticMeshes.TryGet<SStaticMeshComponent>(Entity);
            if (Mesh == nullptr)
            {
                Mesh = SkeletalMeshes.TryGet<SSkeletalMeshComponent>(Entity);
            }
            if (Mesh == nullptr)
            {
                Mesh = DynamicMeshes.TryGet<SDynamicMeshComponent>(Entity);
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

        AudioSourceConsumers.ParallelForEach(Score, kSignificanceParallelGrain);
        PathFollowConsumers.ParallelForEach(Score, kSignificanceParallelGrain);
        PerceptionConsumers.ParallelForEach(Score, kSignificanceParallelGrain);
        AvoidanceConsumers.ParallelForEach(Score, kSignificanceParallelGrain);
    }
}
