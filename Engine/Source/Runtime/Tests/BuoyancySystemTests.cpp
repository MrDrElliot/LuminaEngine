#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Log/Log.h"
#include "Physics/Physics.h"
#include "Physics/PhysicsScene.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/BuoyancyComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Systems/BuoyancySystem.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    void BindBuoyancyTestTransform(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
        Transform.Bind(Registry, Entity);
        Transform.ResetDirtyState();
    }

    struct FBuoyancyTestWorld
    {
        CWorld*                    World  = nullptr;
        TStrongObjectPtr<CWorld>   WorldOwner;
        CBuoyancySystem*           System = nullptr;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }
        Physics::IPhysicsScene& Scene() const { return *World->GetPhysicsScene(); }

        FBuoyancyTestWorld()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("BuoyancySystemTestWorld"), FGuid::New(), OF_Transient);
            WorldOwner = World;
            Registry().Ctx().Emplace<CWorld*>(World);
            Registry().GetSignals<STransformComponent>().OnConstruct.Connect<&BindBuoyancyTestTransform>();
            if (Physics::GetPhysicsContext() == nullptr)
            {
                Physics::Initialize();
            }
            World->EnsurePhysicsScene();

            System = NewObject<CBuoyancySystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            System->SetOwningWorld(World);
            System->ConfigureSystem();
            World->Systems.Add(System);
            FWorldTestAccess::RebuildSystemSchedule(*World);
        }

        ~FBuoyancyTestWorld()
        {
            WorldOwner.Reset();
        }

        ECS::FEntity SpawnWater(float X, bool bBuoyancy)
        {
            const ECS::FEntity Entity = Registry().Create();
            Registry().Emplace<STransformComponent>(Entity).SetLocation(FVector3(X, 0.0f, 0.0f));
            SWaterComponent& Water = Registry().Emplace<SWaterComponent>(Entity);
            Water.Extent = FVector2(10.0f, 10.0f);
            Water.bBuoyancy = bBuoyancy;
            return Entity;
        }

        ECS::FEntity SpawnFloater(float X, float Y)
        {
            const ECS::FEntity Entity = Registry().Create();
            Registry().Emplace<STransformComponent>(Entity).SetLocation(FVector3(X, Y, 0.0f));
            SRigidBodyComponent& Body = Registry().Emplace<SRigidBodyComponent>(Entity);
            Body.BodyType = EBodyType::Dynamic;
            Body.bUseGravity = true;
            Body.LinearDamping = 0.0f;
            Body.AngularDamping = 0.0f;
            Registry().Emplace<SSphereColliderComponent>(Entity).Radius = 0.5f;
            Registry().Emplace<SBuoyancyComponent>(Entity);
            return Entity;
        }

        // Steps physics once, which readies freshly spawned bodies and runs their queued commands.
        void StepPhysics(double Dt)
        {
            ECS::Utils::ResolveAllDirtyTransforms(Registry());
            Scene().Update(Dt);
            Scene().DispatchPendingEvents();
            ECS::Utils::ResolveAllDirtyTransforms(Registry());
        }
    };

    constexpr double kDt = 1.0 / 60.0;
}

TEST(BuoyancySystem, AWaterVolumePastTheSixteenthIsDroppedWithAWarningAndNeverFloatsAnything)
{
    FBuoyancyTestWorld W;

    // One buoyant water and one floater over it, 17 times, so one must miss the 16-plane budget.
    TVector<ECS::FEntity> Floaters;
    for (int32 Index = 0; Index < 17; ++Index)
    {
        const float X = (float)Index * 20.0f;
        W.SpawnWater(X, true);
        Floaters.Add(W.SpawnFloater(X, -1.0f));
    }
    W.StepPhysics(0.0);

    Logging::ClearLogQueue();
    FWorldTestAccess::TickStage(*W.World, EUpdateStage::PrePhysics, kDt);
    W.StepPhysics(kDt);

    bool bWarned = false;
    Logging::FLogQueue& Log = Logging::GetConsoleLogQueue();
    for (int32 Index = 0; Index < Log.Num(); ++Index)
    {
        if (Log[Index].Message.Contains("CBuoyancySystem") && Log[Index].Message.Contains("16"))
        {
            bWarned = true;
            break;
        }
    }
    EXPECT_TRUE(bWarned) << "exceeding the 16-plane budget must log a warning naming the dropped water";

    int32 NumRisen = 0, NumFell = 0;
    for (const ECS::FEntity Floater : Floaters)
    {
        const float VelocityY = W.Scene().GetLinearVelocity(Floater).y;
        VelocityY > 0.0f ? ++NumRisen : ++NumFell;
    }
    EXPECT_EQ(NumRisen, 16) << "every floater over one of the 16 counted waters must be pushed up by buoyancy";
    EXPECT_EQ(NumFell, 1) << "exactly one floater, over the water the cap dropped, gets no buoyancy and just falls";
}
