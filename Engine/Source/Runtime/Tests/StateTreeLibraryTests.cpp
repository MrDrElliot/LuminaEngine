#include <gtest/gtest.h>

#include "AI/StateTree/Library/StateTreeLibraryConditions.h"
#include "AI/StateTree/Library/StateTreeLibraryEvaluators.h"
#include "AI/StateTree/Library/StateTreeLibraryTasks.h"
#include "AI/StateTree/StateTreeTestTypes.h"
#include "NavWorldTestUtil.h"
#include "Physics/Physics.h"
#include "World/Entity/Components/AIStimuliSourceComponent.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/StateTreeComponent.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaStateTreeLibraryTests
{
    using namespace Lumina;
    using namespace Lumina::StateTreeTest;

    using ETrigger = EStateTreeTransitionTrigger;
    using ETarget  = EStateTreeTransitionTarget;
    using ESource  = EStateTreeBindingSource;

    CStateTree* NewTree()
    {
        RegisterPendingTypes();
        return NewObject<CStateTree>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    }

    TEST(StateTreeLibrary, WaitSleepsTheInstanceUntilItsWakeTimeInsteadOfTicking)
    {
        CStateTree* Tree = NewTree();
        AddNode<SStateTreeTestEvaluator>(Tree->Evaluators, "LibTickCounter");
        AddState(*Tree, "LibWaiting");
        AddNode<SStateTreeWaitTask>(Tree->States[0].Tasks, "Wait").Data.Duration = 2.0f;
        AddTransition(Tree->States[0], ETrigger::OnSucceeded, ETarget::State, "LibWaited");
        AddState(*Tree, "LibWaited");
        ASSERT_TRUE(Tree->Compile());

        FStateTreeInstance Instance;
        Instance.Start(Tree->GetRuntime(), nullptr, ECS::FEntity(5, 1), 10.0);
        const SStateTreeTestEvaluatorData& Ticks = *static_cast<const SStateTreeTestEvaluatorData*>(Instance.GetNodeInstanceData(0));
        const int32 TicksAtStart = Ticks.Counter;

        EXPECT_TRUE(Instance.IsWaiting()) << "a waiting state asks for no ticks";
        EXPECT_DOUBLE_EQ(Instance.GetNextWakeTime(), 12.0) << "the authored duration sets the wake time";
        for (double Time = 10.25; Time < 12.0; Time += 0.25)
        {
            Instance.Tick(Time);
        }
        EXPECT_EQ(Ticks.Counter, TicksAtStart) << "ticking before the wake time runs nothing";
        EXPECT_EQ(PathOf(Instance, *Tree->GetRuntime()), "LibWaiting");

        Instance.Tick(12.0);
        EXPECT_EQ(Ticks.Counter, TicksAtStart + 1);
        EXPECT_EQ(PathOf(Instance, *Tree->GetRuntime()), "LibWaited") << "the wake time ticks the wait once and it succeeds";
    }

    TEST(StateTreeLibrary, SetParameterWritesTheParameterLaterBindingsRead)
    {
        CStateTree* Tree = NewTree();
        Tree->Parameters.InitializeAs<SStateTreeTestParameters>().Speed = 3;
        AddState(*Tree, "LibSetSpeed");
        SStateTreeSetParameterTask& Set = AddNode<SStateTreeSetParameterTask>(Tree->States[0].Tasks, "Set");
        Set.Parameter = FName("Speed");
        Set.Data.IntValue = 9;
        Set.bCompleteState = true;
        AddTransition(Tree->States[0], ETrigger::OnSucceeded, ETarget::State, "LibReadSpeed");
        AddState(*Tree, "LibReadSpeed");
        Bind(AddNode<SStateTreeTestTask>(Tree->States[1].Tasks, "Read"), "Input", ESource::Parameter, "", "Speed");
        ASSERT_TRUE(Tree->Compile());

        FStateTreeInstance Instance;
        Instance.Start(Tree->GetRuntime(), nullptr, ECS::FEntity(5, 1), 0.0);
        Instance.Tick(0.1);

        ASSERT_EQ(PathOf(Instance, *Tree->GetRuntime()), "LibReadSpeed");
        const uint16 Read = Tree->GetRuntime()->States[FindState(*Tree->GetRuntime(), "LibReadSpeed")].FirstTask;
        EXPECT_EQ(static_cast<const SStateTreeTestTaskData*>(Instance.GetNodeInstanceData(Read))->Output, 9);
    }

    TEST(StateTreeLibrary, SendEventReachesTheTreesOwnTransitionsNextTick)
    {
        CStateTree* Tree = NewTree();
        AddState(*Tree, "LibSending");
        SStateTreeSendEventTask& Send = AddNode<SStateTreeSendEventTask>(Tree->States[0].Tasks, "Send");
        Send.Data.Event.TagName = FName("StateTreeLibraryTest.Ping");
        AddTransition(Tree->States[0], ETrigger::OnEvent, ETarget::State, "LibPinged").EventTag = Send.Data.Event;
        AddState(*Tree, "LibPinged");
        ASSERT_TRUE(Tree->Compile());

        FStateTreeInstance Instance;
        Instance.Start(Tree->GetRuntime(), nullptr, ECS::FEntity(5, 1), 0.0);
        EXPECT_EQ(PathOf(Instance, *Tree->GetRuntime()), "LibSending") << "the sending task keeps its state running";
        Instance.Tick(0.1);
        EXPECT_EQ(PathOf(Instance, *Tree->GetRuntime()), "LibPinged");
    }

    TEST(StateTreeLibrary, CooldownPassesOncePerDuration)
    {
        CStateTree* Tree = NewTree();
        AddState(*Tree, "LibCooldownRoot", nullptr, EStateTreeStateKind::Group);
        AddTransition(Tree->States[0], ETrigger::OnTick, ETarget::State, "LibCooldownRoot");
        AddState(*Tree, "LibCooled", "LibCooldownRoot");
        AddNode<SStateTreeCooldownCondition>(Tree->States[1].EnterConditions).Data.Duration = 1.0f;
        AddState(*Tree, "LibCoolingDown", "LibCooldownRoot");
        ASSERT_TRUE(Tree->Compile());

        FStateTreeInstance Instance;
        Instance.Start(Tree->GetRuntime(), nullptr, ECS::FEntity(5, 1), 0.0);
        FString Paths = PathOf(Instance, *Tree->GetRuntime());
        for (const double Time : { 0.5, 0.99, 1.0, 1.5, 2.0 })
        {
            Instance.Tick(Time);
            Paths += ", " + PathOf(Instance, *Tree->GetRuntime());
        }
        EXPECT_EQ(Paths, "LibCooldownRoot/LibCooled, LibCooldownRoot/LibCoolingDown, LibCooldownRoot/LibCoolingDown, "
            "LibCooldownRoot/LibCooled, LibCooldownRoot/LibCoolingDown, LibCooldownRoot/LibCooled");
    }

    // A guard on a baked navmesh with real perception and path following, walked by the test since it has no character.
    class FStateTreeLibraryWorldTest : public ::testing::Test
    {
    protected:

        static constexpr float kFrame = 1.0f / 30.0f;
        static constexpr float kWalkSpeed = 5.0f;

        void SetUp() override
        {
            Nav.Create(64.0f);
            Nav.Start();
            ASSERT_TRUE(Nav.WaitReady());
            WorldOwner = Nav.World;
            if (Physics::GetPhysicsContext() == nullptr)
            {
                Physics::Initialize();
            }
            Nav.World->EnsurePhysicsScene();
            Registry().Ctx().Emplace<CWorld*>(Nav.World);
            FWorldTestAccess::KeepOnlySystems(*Nav.World, { "CNavMeshSystem", "CPerceptionSystem", "CStateTreeSystem", "CPathFollowSystem" });
        }

        void TearDown() override
        {
            Nav.Stop();
            WorldOwner.Reset();
        }

        ECS::FRegistry& Registry() const { return Nav.Registry(); }

        // Patrols A and B until it senses anything, turns to it, then chases it and gives up when the chase fails.
        CStateTree* MakeGuardTree() const
        {
            CStateTree* Tree = NewTree();
            AddNode<SStateTreePerceivedTargetEvaluator>(Tree->Evaluators, "Perceived");

            AddState(*Tree, "LibPatrol", nullptr, EStateTreeStateKind::Group);
            AddNode<SStateTreePerceptionSensedCondition>(AddTransition(Tree->States[0], ETrigger::OnTick, ETarget::State, "LibSpot").Conditions);
            Tree->States[0].Transitions[0].Priority = 1;

            AddState(*Tree, "LibPatrolA", "LibPatrol");
            AddNode<SStateTreeMoveToTask>(Tree->States[1].Tasks, "MoveToA").Data.TargetLocation = kPatrolA;
            AddTransition(Tree->States[1], ETrigger::OnSucceeded, ETarget::State, "LibPatrolPause");

            AddState(*Tree, "LibPatrolPause", "LibPatrol");
            AddNode<SStateTreeWaitTask>(Tree->States[2].Tasks, "Pause").Data.Duration = 0.25f;
            AddTransition(Tree->States[2], ETrigger::OnSucceeded, ETarget::State, "LibPatrolB");

            AddState(*Tree, "LibPatrolB", "LibPatrol");
            AddNode<SStateTreeMoveToTask>(Tree->States[3].Tasks, "MoveToB").Data.TargetLocation = kPatrolB;
            AddTransition(Tree->States[3], ETrigger::OnSucceeded, ETarget::State, "LibPatrolA");

            AddState(*Tree, "LibSpot");
            SStateTreeLookAtTask& Look = AddNode<SStateTreeLookAtTask>(Tree->States[4].Tasks, "Look");
            Look.Data.TurnRate = 180.0f;
            Bind(Look, "TargetEntity", ESource::Evaluator, "Perceived", "Target");
            AddTransition(Tree->States[4], ETrigger::OnSucceeded, ETarget::State, "LibChase");

            AddState(*Tree, "LibChase");
            Bind(AddNode<SStateTreeMoveToTask>(Tree->States[5].Tasks, "Chase"), "TargetEntity", ESource::Evaluator, "Perceived", "Target");
            AddTransition(Tree->States[5], ETrigger::OnSucceeded, ETarget::State, "LibCaught");
            AddTransition(Tree->States[5], ETrigger::OnFailed, ETarget::State, "LibGiveUp");

            AddState(*Tree, "LibCaught");
            AddState(*Tree, "LibGiveUp");
            EXPECT_TRUE(Tree->Compile());
            return Tree;
        }

        void SpawnGuard(float SightRadius)
        {
            Guard = Registry().Create();
            // Off the first waypoint, so the first move has somewhere to walk.
            Registry().Emplace<STransformComponent>(Guard).SetLocation(kPatrolA - FVector3(2.0f, 0.0f, 0.0f));
            Registry().Emplace<SPathFollowComponent>(Guard);
            SPerceptionComponent& Sight = Registry().Emplace<SPerceptionComponent>(Guard);
            Sight.EyeOffset = FVector3(0.0f, 1.0f, 0.0f);
            Sight.SightRadius = SightRadius;
            Sight.LoseSightRadius = SightRadius;
            Sight.SightFOVDegrees = 160.0f;
            Sight.UpdateInterval = 0.0f;
            Registry().Emplace<SStateTreeComponent>(Guard).StateTree = MakeGuardTree();
        }

        void SpawnIntruder(const FVector3& Location)
        {
            Intruder = Registry().Create();
            Registry().Emplace<STransformComponent>(Intruder).SetLocation(Location);
            Registry().Emplace<SAIStimuliSourceComponent>(Intruder);
        }

        FString GuardPath() const
        {
            const SStateTreeComponent& Tree = Registry().Get<SStateTreeComponent>(Guard);
            return Tree.Running.Instance != nullptr ? PathOf(*Tree.Running.Instance, *Tree.StateTree->GetRuntime()) : FString();
        }

        // Stands in for a character controller, stepping the guard toward the follower's next corner.
        void Walk() const
        {
            const SPathFollowComponent& Follower = Registry().Get<SPathFollowComponent>(Guard);
            if (!Follower.IsFollowing() || Follower.CurrentCorner >= Follower.CornerCount)
            {
                return;
            }
            const FVector3 Location = Nav.World->GetEntityLocation(Guard);
            const FVector3 ToCorner = Follower.GetNextCorner() - Location;
            const float Distance = Math::Length(ToCorner);
            Nav.World->SetEntityLocation(Guard, Distance <= kWalkSpeed * kFrame ? Follower.GetNextCorner() : Location + ToCorner * (kWalkSpeed * kFrame / Distance));
        }

        // Ticks until the guard's active path is Final, returning every path it passed through.
        FString RunUntil(const char* Final, TFunctionRef<void(int32 NumSeen)> OnNewPath)
        {
            TVector<FString> Seen;
            for (int32 Frame = 0; Frame < 3000 && (Seen.IsEmpty() || Seen.Last() != Final); ++Frame)
            {
                const_cast<SSystemContext&>(FWorldTestAccess::SystemContext(*Nav.World)).Time = Frame * kFrame;
                FWorldTestAccess::TickStage(*Nav.World, EUpdateStage::PrePhysics, kFrame);
                Walk();
                const FString Path = GuardPath();
                if (!Path.IsEmpty() && (Seen.IsEmpty() || Seen.Last() != Path))
                {
                    Seen.Add(Path);
                    OnNewPath(Seen.Num());
                }
            }
            FString Joined;
            for (const FString& Path : Seen)
            {
                Joined += Joined.IsEmpty() ? FString() : FString(", ");
                Joined += Path;
            }
            return Joined;
        }

        static constexpr FVector3 kPatrolA = FVector3(-10.0f, 0.0f, -10.0f);
        static constexpr FVector3 kPatrolB = FVector3(10.0f, 0.0f, -10.0f);

        NavTest::FNavWorld       Nav;
        TStrongObjectPtr<CWorld> WorldOwner;
        ECS::FEntity             Guard;
        ECS::FEntity             Intruder;
    };

    TEST_F(FStateTreeLibraryWorldTest, AGuardPatrolsSpotsAnIntruderAndChasesItDown)
    {
        SpawnGuard(15.0f);
        SpawnIntruder(FVector3(0.0f, 0.0f, -60.0f));

        // Shows the intruder once the guard has walked the whole patrol loop.
        const FString Seen = RunUntil("LibCaught", [&](int32 NumSeen)
        {
            if (NumSeen == 4)
            {
                Registry().Get<STransformComponent>(Intruder).SetLocation(FVector3(0.0f));
            }
        });

        EXPECT_EQ(Seen, "LibPatrol/LibPatrolA, LibPatrol/LibPatrolPause, LibPatrol/LibPatrolB, LibPatrol/LibPatrolA, LibSpot, LibChase, LibCaught");
        EXPECT_LT(Math::FlatDistance(Nav.World->GetEntityLocation(Guard), FVector3(0.0f)), 1.0f) << "the chase ended at the intruder";
    }

    TEST_F(FStateTreeLibraryWorldTest, ChasingAnIntruderOffTheNavmeshFailsTheMove)
    {
        SpawnGuard(60.0f);
        SpawnIntruder(FVector3(0.0f, 0.0f, 45.0f));

        EXPECT_EQ(RunUntil("LibGiveUp", [](int32) {}), "LibPatrol/LibPatrolA, LibSpot, LibChase, LibGiveUp");
        const FVector3 Forward = Nav.World->GetEntityTransform(Guard).GetForward();
        EXPECT_GT(Math::Dot(Math::Normalize(Math::Flat(FVector3(0.0f, 0.0f, 45.0f) - Nav.World->GetEntityLocation(Guard))), Forward), 0.9f)
            << "the spot turned the guard to face the intruder";
    }
}
