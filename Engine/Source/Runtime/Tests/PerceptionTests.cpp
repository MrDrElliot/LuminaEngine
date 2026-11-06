#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Physics/Physics.h"
#include "World/World.h"
#include "World/Entity/Components/AIStimuliSourceComponent.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/PerceptionSystem.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    void BindPerceptionTransform(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
        Transform.Bind(Registry, Entity);
        Transform.ResetDirtyState();
    }

    // A bare world with a physics scene for the sight ray, one guard at the origin and one target to move around it.
    struct FSightWorld
    {
        CWorld*            World  = nullptr;
        SPerceptionSystem* System = nullptr;

        // Released before the harness stops the task system, which the world's physics scene still runs on.
        TStrongObjectPtr<CWorld> WorldOwner;
        ECS::FEntity       Guard;
        ECS::FEntity       Target;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }

        FSightWorld()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("PerceptionTestWorld"), FGuid::New(), OF_Transient);
            WorldOwner = World;
            ECS::FRegistry& R = Registry();
            R.Ctx().Emplace<CWorld*>(World);
            R.GetSignals<STransformComponent>().OnConstruct.Connect<&BindPerceptionTransform>();
            // The engine starts physics at boot, and the test harness does not, so the sight ray needs it started here.
            if (Physics::GetPhysicsContext() == nullptr)
            {
                Physics::Initialize();
            }
            World->EnsurePhysicsScene();

            Guard = R.Create();
            R.Emplace<STransformComponent>(Guard);
            SPerceptionComponent& Sight = R.Emplace<SPerceptionComponent>(Guard);
            Sight.EyeOffset      = FVector3(0.0f);
            Sight.UpdateInterval = 0.0f;
            Sight.ForgetTime     = 0.0f;

            Target = R.Create();
            R.Emplace<STransformComponent>(Target);
            R.Emplace<SAIStimuliSourceComponent>(Target).SightTargetOffset = FVector3(0.0f);

            System = NewObject<SPerceptionSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            System->SetOwningWorld(World);
            System->ConfigureSystem();
            System->OnStartup(SExclusiveContext(FWorldTestAccess::SystemContext(*World)));
        }

        ~FSightWorld()
        {
            System->OnTeardown(SExclusiveContext(FWorldTestAccess::SystemContext(*World)));
            System->SetOwningWorld(nullptr);
            WorldOwner.Reset();
        }

        FVector3 Forward() const
        {
            return Registry().Get<STransformComponent>(Guard).GetWorldTransformCached().GetForward();
        }

        void PlaceTarget(const FVector3& Location)
        {
            Registry().Get<STransformComponent>(Target).SetLocation(Location);
        }

        bool Sees()
        {
            SSystemContext Context = FWorldTestAccess::SystemContext(*World);
            Context.DeltaTime = 0.1;
            System->OnUpdate(Context);
            const SPerceptionComponent& Sight = Registry().Get<SPerceptionComponent>(Guard);
            const int32 Index = Sight.Find(Target);
            return Index >= 0 && (Sight.PerceivedTargets[Index].ActiveSenses & (uint8)EAISenseChannel::Sight) != 0;
        }
    };
}

// Losing sight is a cone as well as a radius, so a target that walks behind the guard is gone without leaving the radius.
TEST(Perception, ATargetBehindTheGuardIsLostInsideTheLoseSightRadius)
{
    FSightWorld W;
    const FVector3 Forward = W.Forward();

    W.PlaceTarget(Forward * 10.0f);
    ASSERT_TRUE(W.Sees()) << "a target straight ahead and inside SightRadius is seen";

    W.PlaceTarget(-Forward * 10.0f);
    EXPECT_FALSE(W.Sees()) << "behind the guard is outside the lose-sight cone even though it is well inside LoseSightRadius";
}

// The lose-sight cone is wider than the acquire cone, so a seen target just past the acquire edge is still held.
TEST(Perception, ASeenTargetJustOutsideTheAcquireConeIsHeld)
{
    FSightWorld W;
    const FVector3 Forward = W.Forward();
    SPerceptionComponent& Sight = W.Registry().Get<SPerceptionComponent>(W.Guard);
    Sight.SightFOVDegrees     = 90.0f;
    Sight.LoseSightFOVDegrees = 120.0f;

    W.PlaceTarget(Forward * 10.0f);
    ASSERT_TRUE(W.Sees());

    // 50 degrees off the forward axis, past the 45 degree acquire half-angle but inside the 60 degree lose half-angle.
    const FVector3 Side = Math::Normalize(Math::Cross(Forward, FVector3(0.0f, 1.0f, 0.0f)));
    const float Angle = Math::Radians(50.0f);
    W.PlaceTarget((Forward * Math::Cos(Angle) + Side * Math::Sin(Angle)) * 10.0f);
    EXPECT_TRUE(W.Sees());
}

// A lose radius set inside the sight radius must not drop a target the sight radius just took, or zero memory loops forever.
TEST(Perception, ALoseSightRadiusSmallerThanTheSightRadiusDoesNotFlicker)
{
    FSightWorld W;
    SPerceptionComponent& Sight = W.Registry().Get<SPerceptionComponent>(W.Guard);
    Sight.SightRadius     = 20.0f;
    Sight.LoseSightRadius = 10.0f;

    W.PlaceTarget(W.Forward() * 15.0f);
    for (int32 Scan = 0; Scan < 6; ++Scan)
    {
        EXPECT_TRUE(W.Sees()) << "scan " << Scan << " lost a target still inside SightRadius";
    }
}

