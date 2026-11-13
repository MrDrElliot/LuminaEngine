#include <gtest/gtest.h>

#include "AI/StateTree/StateTreeTestTypes.h"
#include "Core/Object/ObjectCore.h"
#include "HierarchicalTags/HierarchicalTag.h"
#include "UI/Tools/AssetEditors/StateTree/StateTreeDebugger.h"
#include "World/World.h"
#include "World/Entity/Components/StateTreeComponent.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaStateTreeDebuggerTests
{
    using namespace Lumina;
    using namespace Lumina::StateTreeTest;

    using ETrigger = EStateTreeTransitionTrigger;
    using ETarget  = EStateTreeTransitionTarget;
    using EKind    = EStateTreeTraceKind;

    class FStateTreeDebuggerTest : public ::testing::Test
    {
    protected:

        // DebugRoot holds DebugIdle, which an alert moves to DebugAlerted, whose task fails on its first tick.
        void SetUp() override
        {
            RegisterPendingTypes();
            Tree = NewObject<CStateTree>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            AddState(*Tree, "DebugRoot", nullptr, EStateTreeStateKind::Group);
            AddState(*Tree, "DebugIdle", "DebugRoot");
            AddState(*Tree, "DebugAlerted", "DebugRoot");
            AddNode<SStateTreeTestTask>(Tree->States[1].Tasks, "Wait");
            SStateTreeTestTask& Fail = AddNode<SStateTreeTestTask>(Tree->States[2].Tasks, "Fail");
            Fail.TicksToFinish = 1;
            Fail.Result = EStateTreeRunStatus::Failed;
            AddTransition(Tree->States[1], ETrigger::OnEvent, ETarget::State, "DebugAlerted").EventTag.TagName = FName("StateTreeDebugTest.Alert");
            ASSERT_TRUE(Tree->Compile());

            World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            Entity = World->GetEntityRegistry().Create();
            FStateTreeRunningInstance& Running = World->GetEntityRegistry().Emplace<SStateTreeComponent>(Entity).Running;
            Running.Tree = Tree;
            Running.Instance = MakeUnique<FStateTreeInstance>();
            Running.Instance->Start(Tree->GetRuntime(), World, Entity, 0.0);
        }

        FStateTreeInstance& Instance() const { return *World->GetEntityRegistry().Get<SStateTreeComponent>(Entity).Running.Instance; }

        void TickWithEvent(double Time, const char* Event) const
        {
            SHierarchicalTag Tag;
            Tag.TagName = FName(Event);
            ASSERT_TRUE(Instance().SendEvent(Tag.GetId()));
            Instance().Tick(Time);
        }

        TOptional<EStateTreeRunStatus> StatusOf(const FStateTreeDebugger& Debugger, const char* State) const
        {
            return Debugger.GetStateStatus(*Tree, Algo::IndexOf(Tree->States, FName(State), &SStateTreeState::Name));
        }

        CStateTree*  Tree = nullptr;
        CWorld*      World = nullptr;
        ECS::FEntity Entity = ECS::NullEntity;
    };

    TEST_F(FStateTreeDebuggerTest, NothingIsRecordedUntilADebuggerWatchesAndTheTraceStaysBounded)
    {
        TickWithEvent(1.0, "StateTreeDebugTest.Alert");
        EXPECT_EQ(Instance().GetTrace(), nullptr) << "an unwatched instance allocates no trace";

        Instance().Start(Tree->GetRuntime(), World, Entity, 2.0);
        {
            FStateTreeDebugger Debugger;
            Debugger.SetTarget(World, Entity);
            ASSERT_NE(Instance().GetTrace(), nullptr);
            EXPECT_TRUE(Instance().GetTrace()->GetRecords().IsEmpty()) << "nothing from before the debugger attached";

            for (int32 Tick = 0; Tick <= FStateTreeTrace::kCapacity; ++Tick)
            {
                TickWithEvent(3.0 + Tick, "StateTreeDebugTest.Other");
            }
            EXPECT_EQ(Instance().GetTrace()->GetRecords().Num(), FStateTreeTrace::kCapacity);
            EXPECT_EQ(Instance().GetTrace()->GetRecords().Last().Time, 3.0 + FStateTreeTrace::kCapacity);

            Instance().Start(Tree->GetRuntime(), World, Entity, 1000.0);
            EXPECT_EQ(Instance().GetTrace()->GetRecords().Num(), 2) << "a restart keeps only the new run's two enters";
        }
        EXPECT_EQ(Instance().GetTrace(), nullptr) << "closing the debugger stops tracing";
    }

    TEST_F(FStateTreeDebuggerTest, TheTimelineRecordsEveryChangeAndScrubbingShowsThePathAtAPastFrame)
    {
        FStateTreeDebugger Debugger;
        Debugger.SetTarget(World, Entity);
        EXPECT_EQ(StatusOf(Debugger, "DebugIdle"), EStateTreeRunStatus::Running);

        TickWithEvent(0.5, "StateTreeDebugTest.Other");
        TickWithEvent(1.0, "StateTreeDebugTest.Alert");
        Instance().Tick(2.0);

        const FStateTreeRuntime& Runtime = *Tree->GetRuntime();
        const TRingBuffer<FStateTreeTraceRecord>& Records = Instance().GetTrace()->GetRecords();
        const struct { double Time; EKind Kind; const char* State; EStateTreeRunStatus Status; } Expected[] =
        {
            { 0.5, EKind::Event,      nullptr,        EStateTreeRunStatus::Running },
            { 1.0, EKind::Event,      nullptr,        EStateTreeRunStatus::Running },
            { 1.0, EKind::Transition, "DebugAlerted", EStateTreeRunStatus::Running },
            { 1.0, EKind::Exit,       "DebugIdle",    EStateTreeRunStatus::Running },
            { 1.0, EKind::Enter,      "DebugAlerted", EStateTreeRunStatus::Running },
            { 2.0, EKind::Exit,       "DebugAlerted", EStateTreeRunStatus::Failed },
            { 2.0, EKind::Exit,       "DebugRoot",    EStateTreeRunStatus::Running },
            { 2.0, EKind::Finished,   nullptr,        EStateTreeRunStatus::Failed },
        };
        ASSERT_EQ(Records.Num(), (int32)std::size(Expected));
        for (int32 Index = 0; Index < Records.Num(); ++Index)
        {
            const FStateTreeTraceRecord& Record = Records[Index];
            EXPECT_EQ(Record.Time, Expected[Index].Time) << Index;
            EXPECT_EQ(Record.Kind, Expected[Index].Kind) << Index;
            EXPECT_EQ(Record.State, Expected[Index].State != nullptr ? FindState(Runtime, Expected[Index].State) : kStateTreeNoIndex) << Index;
            EXPECT_EQ(Record.Status, Expected[Index].Status) << Index;
        }

        EXPECT_FALSE(StatusOf(Debugger, "DebugAlerted").IsSet()) << "live, the finished tree has no path";

        Debugger.ScrubTo(0.75);
        EXPECT_EQ(StatusOf(Debugger, "DebugRoot"), EStateTreeRunStatus::Running);
        EXPECT_EQ(StatusOf(Debugger, "DebugIdle"), EStateTreeRunStatus::Running);
        EXPECT_FALSE(StatusOf(Debugger, "DebugAlerted").IsSet());

        Debugger.ScrubTo(1.0);
        EXPECT_FALSE(StatusOf(Debugger, "DebugIdle").IsSet());
        EXPECT_EQ(StatusOf(Debugger, "DebugAlerted"), EStateTreeRunStatus::Running);

        Debugger.ScrubTo(0.25);
        EXPECT_FALSE(StatusOf(Debugger, "DebugIdle").IsSet()) << "nothing was traced that early";

        Debugger.ResumeLive();
        EXPECT_FALSE(StatusOf(Debugger, "DebugRoot").IsSet());
    }
}
