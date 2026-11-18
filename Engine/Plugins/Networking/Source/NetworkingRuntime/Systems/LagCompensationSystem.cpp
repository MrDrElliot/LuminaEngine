#include "RuntimePCH.h"
#include "LagCompensationSystem.h"

#include "Components/RepTransformComponent.h"
#include "Config/NetworkSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Systems/NetMovementInterpSystem.h"
#include "World/World.h"

namespace Lumina
{
    void CLagCompensationSystem::Configure()
    {
        QueuesNoCommands();
        RequireUpdate(EUpdateStage::FrameEnd);
    }

    bool CLagCompensationSystem::HasWork(const SSystemContext& Context)
    {
        // Only a host judges shots, and a world with no hitboxes has nothing to remember.
        const CWorld* World = GetOwningWorld();
        return World != nullptr && World->IsNetServer() && !Hitboxes.View().IsEmpty();
    }

    void CLagCompensationSystem::OnUpdate(const SSystemContext& Context)
    {
        FLagCompensationHistory& Record = *History;
        const CNetworkSettings* Settings = GetDefault<CNetworkSettings>();
        Record.MaxRewindSeconds = Settings->MaxLagCompensationSeconds;

        // A little under the interval, so a frame landing a hair early still records rather than skipping to the next.
        const double Interval = Settings->LagCompensationRecordRate > 0.0f ? 0.9 / Settings->LagCompensationRecordRate : 0.0;
        if (Record.Count > 0 && Context.GetTime() - Record.GetNewestTime() < Interval)
        {
            return;
        }

        FLagCompensationFrame& Frame = Record.BeginRecord(Context.GetTime());
        Frame.Samples.SetNum(Hitboxes.View().Num());

        // Each worker claims its own slots, so the frame fills in parallel and is sorted by entity afterward.
        std::atomic<int32> Next{ 0 };
        Hitboxes.ParallelForEach([&](ECS::FEntity Entity, const SLagCompensationComponent& Hitbox, const STransformComponent& Transform)
        {
            const FTransform World = Transform.GetWorldTransform();
            const FVector3 Up      = World.GetRotation() * FVector3(0.0f, 1.0f, 0.0f);
            const FVector3 Scale   = World.GetScale();
            const FVector3 Center  = World.GetLocation() + World.GetRotation() * (Hitbox.Center * Scale);
            const float    Along   = Hitbox.HalfHeight * Scale.y;

            FLagCompensationSample& Sample = Frame.Samples[Next.fetch_add(1, std::memory_order_relaxed)];
            Sample.Entity = Entity;
            Sample.A      = Center - Up * Along;
            Sample.B      = Center + Up * Along;
            Sample.Radius = Hitbox.Radius * Math::Max(Scale.x, Scale.z);
        }, 64u);
        Frame.Samples.SetNum(Next.load(std::memory_order_relaxed));
        Record.FinishRecord();
    }

    double CLagCompensationLibrary::GetViewTime(CWorld* World, ECS::FEntity Target)
    {
        if (World == nullptr)
        {
            return 0.0;
        }
        ECS::FRegistry& Registry = World->GetEntityRegistry();

        // A host draws everything as it is, so its view time is now.
        if (World->IsNetServer() || World->GetNetMode() == ENetMode::Standalone)
        {
            const FLagCompensationHistory* History = Registry.Ctx().Find<FLagCompensationHistory>();
            return History != nullptr ? History->GetNewestTime() : 0.0;
        }

        const FNetInterpClock* Clock = Registry.Ctx().Find<FNetInterpClock>();
        if (Clock == nullptr)
        {
            return 0.0;
        }
        const FRepTransform* Rep = Registry.IsValid(Target) ? Registry.TryGet<FRepTransform>(Target) : nullptr;
        const double Delay = Rep != nullptr && Rep->SmoothedInterpDelay > 0.0 ? Rep->SmoothedInterpDelay : (double)GetDefault<CNetworkSettings>()->InterpDelay;
        return Clock->ServerPlaybackTime - Delay;
    }

    SLagCompensatedHit CLagCompensationLibrary::Raycast(CWorld* World, double ViewTime, FVector3 Origin, FVector3 Direction, float MaxDistance, float Thickness, ECS::FEntity Ignore)
    {
        const FLagCompensationHistory* History = World != nullptr ? World->GetEntityRegistry().Ctx().Find<FLagCompensationHistory>() : nullptr;
        return History != nullptr ? LagCompensation::Raycast(*History, ViewTime, Origin, Direction, MaxDistance, Thickness, Ignore) : SLagCompensatedHit();
    }

    TVector<ECS::FEntity> CLagCompensationLibrary::OverlapSphere(CWorld* World, double ViewTime, FVector3 Center, float Radius)
    {
        TVector<ECS::FEntity> Out;
        if (const FLagCompensationHistory* History = World != nullptr ? World->GetEntityRegistry().Ctx().Find<FLagCompensationHistory>() : nullptr)
        {
            LagCompensation::OverlapSphere(*History, ViewTime, Center, Radius, Out);
        }
        return Out;
    }
}
