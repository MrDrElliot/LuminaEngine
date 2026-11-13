#include <gtest/gtest.h>

#include "AI/StateTree/StateTreeTestTypes.h"
#include "Containers/Algorithm.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectCore.h"
#include "Memory/MemoryTracking.h"
#include "World/ECS/CommandBus.h"
#include "World/Entity/Components/StateTreeComponent.h"
#include "World/World.h"
#include "WorldTestAccess.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaStateTreeSystemTests
{
    using namespace Lumina;
    using namespace Lumina::StateTreeTest;

    constexpr double kFrame = 1.0 / 60.0;

    // More than one parallel chunk per group, so the system splits the walk across workers.
    constexpr int32 kPerGroup = 200;

    class FStateTreeSystemTest : public ::testing::Test
    {
    protected:

        void SetUp() override
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            World->GetEntityRegistry().Ctx().Emplace<CWorld*>(World);
            FWorldTestAccess::KeepOnlySystems(*World, { "CStateTreeSystem" });
        }

        // One state running one test task, which the caller configures.
        CStateTree* MakeTree(bool bNeedsTick, float WakeAfter, FStateTreeTestLog* Log = nullptr)
        {
            CStateTree* Tree = NewObject<CStateTree>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            AddState(*Tree, "Root");
            SStateTreeTestTask& Task = AddNode<SStateTreeTestTask>(Tree->States[0].Tasks, "Task");
            Task.bNeedsTick = bNeedsTick;
            Task.WakeAfter = WakeAfter;
            Task.Log = Log;
            Bind(Task, "Input", EStateTreeBindingSource::Parameter, "", "Speed");
            Tree->Parameters.InitializeAs<SStateTreeTestParameters>().Speed = 3;
            EXPECT_TRUE(Tree->Compile());
            return Tree;
        }

        TVector<ECS::FEntity> Spawn(CStateTree* Tree, int32 Count)
        {
            TVector<ECS::FEntity> Entities;
            for (int32 Index = 0; Index < Count; ++Index)
            {
                const ECS::FEntity Entity = World->GetEntityRegistry().Create();
                World->GetEntityRegistry().Emplace<SStateTreeComponent>(Entity).StateTree = Tree;
                Entities.Add(Entity);
            }
            return Entities;
        }

        SStateTreeComponent& Component(ECS::FEntity Entity) const { return World->GetEntityRegistry().Get<SStateTreeComponent>(Entity); }

        const SStateTreeTestTaskData& TaskData(ECS::FEntity Entity) const
        {
            const SStateTreeComponent& Tree = Component(Entity);
            return *static_cast<const SStateTreeTestTaskData*>(Tree.Running.Instance->GetNodeInstanceData(Tree.StateTree->GetRuntime()->States[0].FirstTask));
        }

        bool AllTicked(const TVector<ECS::FEntity>& Entities, int32 TickCount) const
        {
            return Algo::AllOf(Entities, [&](ECS::FEntity Entity) { return TaskData(Entity).TickCount == TickCount; });
        }

        void Tick(double Time) const
        {
            // TickStage leaves the world clock alone, and the system wakes instances against it.
            const_cast<SSystemContext&>(FWorldTestAccess::SystemContext(*World)).Time = Time;
            FWorldTestAccess::TickStage(*World, EUpdateStage::PrePhysics, kFrame);
        }

        static uint64 TotalAllocations()
        {
            Memory::FMemoryCategoryStats Stats[256];
            const uint32 Num = Memory::GetCategoryStats(Stats, 256);
            return Algo::Accumulate(Stats, Stats + Num, uint64(0), &Memory::FMemoryCategoryStats::TotalAllocs);
        }

        static uint32 PokeTag()
        {
            SHierarchicalTag Tag;
            Tag.TagName = FName("StateTreeSystemTest.Poke");
            return Tag.GetId();
        }

        CWorld* World = nullptr;
    };

    TEST_F(FStateTreeSystemTest, ActiveInstancesTickAndWaitingOnesSleepUntilTheirWakeTimeOrAnEvent)
    {
        const TVector<ECS::FEntity> Busy = Spawn(MakeTree(true, 0.0f), kPerGroup);
        const TVector<ECS::FEntity> Timed = Spawn(MakeTree(false, 1.0f), kPerGroup);
        const TVector<ECS::FEntity> Sleepers = Spawn(MakeTree(false, 0.0f), kPerGroup);

        Component(Busy[0]).Parameters.InitializeAs<SStateTreeTestParameters>().Speed = 7;

        Tick(0.0);
        ASSERT_TRUE(Algo::AllOf(Busy, [&](ECS::FEntity Entity) { return Component(Entity).Running.Instance != nullptr; })) << "the first tick starts every instance";
        EXPECT_EQ(TaskData(Busy[0]).Output, 7) << "the component's parameters replace the tree's defaults";
        EXPECT_EQ(TaskData(Busy[1]).Output, 3);

        for (int32 Frame = 1; Frame <= 5; ++Frame)
        {
            Tick(Frame * 0.1);
        }
        EXPECT_TRUE(AllTicked(Busy, 5));
        EXPECT_TRUE(AllTicked(Timed, 0)) << "an instance waiting on a wake time is skipped before it";
        EXPECT_TRUE(AllTicked(Sleepers, 0)) << "an instance with nothing to tick sleeps";

        const uint32 Poke = PokeTag();
        for (int32 Index = 0; Index < kPerGroup; Index += 2)
        {
            ASSERT_TRUE(Component(Sleepers[Index]).SendEvent(Poke));
        }
        Tick(0.6);
        for (int32 Index = 0; Index < kPerGroup; ++Index)
        {
            EXPECT_EQ(TaskData(Sleepers[Index]).TickCount, Index % 2 == 0 ? 1 : 0) << "only an instance sent an event wakes";
        }
        EXPECT_TRUE(AllTicked(Timed, 0));

        Tick(1.0);
        EXPECT_TRUE(Algo::AllOf(Timed, [&](ECS::FEntity Entity) { return Component(Entity).Running.Instance->GetStatus() == EStateTreeRunStatus::Succeeded; }))
            << "the wake time ticks the task once and it completes";
        EXPECT_TRUE(AllTicked(Busy, 7));
    }

    TEST_F(FStateTreeSystemTest, TasksDestroyingTheirOwnerFromWorkerChunksApplyAtTheSyncPoint)
    {
        CStateTree* DestroyTree = NewObject<CStateTree>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        AddState(*DestroyTree, "Root");
        AddNode<SStateTreeDestroyOwnerTask>(DestroyTree->States[0].Tasks, "Destroy");
        ASSERT_TRUE(DestroyTree->Compile());
        const TVector<ECS::FEntity> Doomed = Spawn(DestroyTree, kPerGroup);
        const TVector<ECS::FEntity> Busy = Spawn(MakeTree(true, 0.0f), kPerGroup);

        // Counts destroys that ran where a worker chunk would race other readers.
        int32 ImmediateDestroys = 0;
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        ECS::FSignalConnection Connection = Registry.OnEntityDestroyed().Connect([](void* Payload, ECS::FRegistry&, ECS::FEntity)
        {
            *static_cast<int32*>(Payload) += ECS::FCommandBus::ShouldDefer() ? 1 : 0;
        }, &ImmediateDestroys);

        Tick(0.0);
        Connection.Release();

        EXPECT_EQ(ImmediateDestroys, 0) << "every destroy waited for the command bus flush";
        EXPECT_TRUE(Algo::NoneOf(Doomed, [&](ECS::FEntity Entity) { return Registry.IsValid(Entity); })) << "the flush applied every owner's destroy";
        EXPECT_TRUE(Algo::AllOf(Busy, [&](ECS::FEntity Entity) { return Registry.IsValid(Entity) && Component(Entity).Running.Instance != nullptr; }));
    }

    TEST_F(FStateTreeSystemTest, DestroyingARunningTreeExitsItsActiveTasks)
    {
        FStateTreeTestLog DestroyedLog;
        FStateTreeTestLog RemovedLog;
        const ECS::FEntity Destroyed = Spawn(MakeTree(true, 0.0f, &DestroyedLog), 1)[0];
        const ECS::FEntity Removed = Spawn(MakeTree(true, 0.0f, &RemovedLog), 1)[0];
        Tick(0.0);
        Tick(kFrame);
        ASSERT_EQ(DestroyedLog.Lines.Last(), "Tick Task");

        World->GetEntityRegistry().Destroy(Destroyed);
        World->GetEntityRegistry().Remove<SStateTreeComponent>(Removed);

        EXPECT_EQ(DestroyedLog.Lines.Last(), "Exit Task") << "destroying the entity exits the active task";
        EXPECT_EQ(RemovedLog.Lines.Last(), "Exit Task") << "removing the component exits the active task";
    }

    TEST_F(FStateTreeSystemTest, RecompilingATreeRestartsItsInstancesOnTheNewRuntime)
    {
        CStateTree* Tree = MakeTree(true, 0.0f);
        const TVector<ECS::FEntity> Busy = Spawn(Tree, 4);
        Tick(0.0);
        Tick(kFrame);

        Tree->PostPropertyChange(nullptr);
        Tick(2.0 * kFrame);

        ASSERT_TRUE(Algo::AllOf(Busy, [&](ECS::FEntity Entity) { return Component(Entity).Running.Instance->GetStatus() == EStateTreeRunStatus::Running; }))
            << "the system restarts an instance whose runtime was superseded instead of leaving it stopped";
        EXPECT_TRUE(AllTicked(Busy, 0)) << "the restart entered fresh task data on the new runtime";
    }

    TEST_F(FStateTreeSystemTest, TickingStartedInstancesMakesNoHeapAllocations)
    {
        if (!Memory::IsTrackingEnabled())
        {
            GTEST_SKIP() << "allocation tracking is off in this build";
        }
        const TVector<ECS::FEntity> Busy = Spawn(MakeTree(true, 0.0f), kPerGroup);
        const TVector<ECS::FEntity> Sleepers = Spawn(MakeTree(false, 0.0f), kPerGroup);

        const uint32 Poke = PokeTag();
        int32 Frame = 0;
        auto TickWindow = [&]
        {
            const uint64 Before = TotalAllocations();
            for (int32 Index = 0; Index < 10; ++Index, ++Frame)
            {
                Component(Sleepers[Frame]).SendEvent(Poke);
                Tick(Frame * kFrame);
            }
            return TotalAllocations() - Before;
        };

        // Warms the job workers' fiber stack pool, which they fill lazily and not always inside a tick.
        TickWindow();
        TickWindow();

        // An allocation on the tick path shows in every window, while a worker topping up its pool shows in one.
        const uint64 Quietest = Math::Min(TickWindow(), Math::Min(TickWindow(), TickWindow()));
        EXPECT_EQ(Quietest, 0u);
        EXPECT_TRUE(AllTicked(Busy, Frame - 1));
    }
}
