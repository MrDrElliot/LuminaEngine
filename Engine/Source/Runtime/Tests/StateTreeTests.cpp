#include <gtest/gtest.h>

#include "AI/StateTree/StateTreeTestTypes.h"
#include "Core/Object/ObjectCore.h"
#include "HierarchicalTags/HierarchicalTag.h"
#include "Memory/MemoryTracking.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaStateTreeTests
{
    using namespace Lumina;
    using namespace Lumina::StateTreeTest;

    using EKind    = EStateTreeStateKind;
    using ETrigger = EStateTreeTransitionTrigger;
    using ETarget  = EStateTreeTransitionTarget;
    using ESource  = EStateTreeBindingSource;

    class FStateTreeTest : public ::testing::Test
    {
    protected:

        void SetUp() override
        {
            RegisterPendingTypes();
            Tree = NewTree();
        }

        static CStateTree* NewTree()
        {
            return NewObject<CStateTree>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        }

        SStateTreeState& State(const char* Name, CStateTree* Owner = nullptr)
        {
            Owner = Owner != nullptr ? Owner : Tree;
            return Owner->States[Algo::IndexOf(Owner->States, FName(Name), &SStateTreeState::Name)];
        }

        SStateTreeTestTask& AddTask(const char* StateName, const char* TaskName, CStateTree* Owner = nullptr)
        {
            SStateTreeTestTask& Task = AddNode<SStateTreeTestTask>(State(StateName, Owner).Tasks, TaskName);
            Task.Log = &Log;
            return Task;
        }

        SStateTreeTestCondition& AddEnterCondition(const char* StateName, bool bPass)
        {
            SStateTreeTestCondition& Condition = AddNode<SStateTreeTestCondition>(State(StateName).EnterConditions);
            Condition.bPass = bPass;
            return Condition;
        }

        void Start(double Time = 0.0, const FInstancedStruct* Parameters = nullptr)
        {
            ASSERT_TRUE(Tree->Compile());
            Instance.Start(Tree->GetRuntime(), nullptr, ECS::FEntity(5, 1), Time, Parameters);
        }

        FString Path() const { return PathOf(Instance, *Tree->GetRuntime()); }

        FString Lines() const
        {
            FString Joined;
            for (const FString& Line : Log.Lines)
            {
                Joined += Joined.IsEmpty() ? "" : ", ";
                Joined += Line;
            }
            return Joined;
        }

        SStateTreeTestTaskData& TaskData(const char* StateName, int32 TaskIndex = 0)
        {
            return TaskData(FindState(*Tree->GetRuntime(), StateName), TaskIndex);
        }

        SStateTreeTestTaskData& TaskData(uint16 StateIndex, int32 TaskIndex = 0)
        {
            return *static_cast<SStateTreeTestTaskData*>(Instance.GetNodeInstanceData((uint16)(Tree->GetRuntime()->States[StateIndex].FirstTask + TaskIndex)));
        }

        SStateTreeTestTaskData& LeafTaskData() { return TaskData(Instance.GetActivePath().Last().State); }

        static uint32 Tag(const char* Name)
        {
            SHierarchicalTag Tag;
            Tag.TagName = FName(Name);
            return Tag.GetId();
        }

        // Log outlives Instance, whose destructor exits the active tasks into it.
        FStateTreeTestLog  Log;
        CStateTree*        Tree = nullptr;
        FStateTreeInstance Instance;
    };

    TEST_F(FStateTreeTest, SelectionEntersTheFirstChildWhoseConditionsPassThroughNestedGroups)
    {
        AddState(*Tree, "StateTreeRoot", nullptr, EKind::Group);
        AddState(*Tree, "Blocked", "StateTreeRoot");
        AddState(*Tree, "Empty", "StateTreeRoot", EKind::Group);
        AddState(*Tree, "Combat", "StateTreeRoot", EKind::Group);
        AddState(*Tree, "Inverted", "Combat");
        AddState(*Tree, "Either", "Combat");
        AddState(*Tree, "Later", "Combat");
        AddEnterCondition("Blocked", false);
        AddEnterCondition("Inverted", true).bInvert = true;
        AddEnterCondition("Either", false);
        AddEnterCondition("Either", true).Operand = EStateTreeConditionOperand::Or;
        AddEnterCondition("Either", true);
        AddTask("Either", "EitherTask");
        AddTask("Later", "LaterTask");

        Start();

        EXPECT_EQ(Instance.GetStatus(), EStateTreeRunStatus::Running);
        EXPECT_EQ(Path(), "StateTreeRoot/Combat/Either") << "a group with no selectable child is skipped, and (false or true) and true passes";
        EXPECT_EQ(Lines(), "Enter EitherTask");
    }

    TEST_F(FStateTreeTest, AStateWhoseChildrenAllFailIsTheLeafButAGroupIsNot)
    {
        AddState(*Tree, "Group", nullptr, EKind::Group);
        AddState(*Tree, "GroupChild", "Group");
        AddState(*Tree, "Parent");
        AddState(*Tree, "ParentChild", "Parent");
        AddEnterCondition("GroupChild", false);
        AddEnterCondition("ParentChild", false);

        Start();

        EXPECT_EQ(Path(), "Parent");
    }

    TEST_F(FStateTreeTest, CompletionTransitionsFollowTheTaskResult)
    {
        AddState(*Tree, "A");
        AddState(*Tree, "B");
        AddState(*Tree, "C");
        AddState(*Tree, "Wrong");
        AddTask("A", "TaskA").TicksToFinish = 1;
        SStateTreeTestTask& TaskB = AddTask("B", "TaskB");
        TaskB.TicksToFinish = 1;
        TaskB.Result = EStateTreeRunStatus::Failed;
        AddTask("C", "TaskC").TicksToFinish = 2;
        AddTransition(State("A"), ETrigger::OnFailed, ETarget::State, "Wrong");
        AddTransition(State("A"), ETrigger::OnSucceeded, ETarget::State, "B");
        AddTransition(State("B"), ETrigger::OnSucceeded, ETarget::State, "Wrong");
        AddTransition(State("B"), ETrigger::OnFailed, ETarget::State, "C");
        AddTransition(State("C"), ETrigger::OnCompleted, ETarget::Succeeded);

        Start();
        Instance.Tick(0.1);
        EXPECT_EQ(Path(), "B");
        Instance.Tick(0.2);
        EXPECT_EQ(Path(), "C");
        Instance.Tick(0.3);
        EXPECT_EQ(Path(), "C") << "TaskC needs two ticks";
        EXPECT_EQ(Instance.Tick(0.4), EStateTreeRunStatus::Succeeded);
        EXPECT_TRUE(Instance.GetActivePath().IsEmpty());
    }

    TEST_F(FStateTreeTest, ATaskResultPropagatesToTheNearestAncestorThatHandlesIt)
    {
        AddState(*Tree, "Parent");
        AddState(*Tree, "Child", "Parent");
        AddState(*Tree, "Next");
        AddTask("Child", "ChildTask").TicksToFinish = 1;
        SStateTreeTestTask& Failing = AddTask("Next", "NextTask");
        Failing.TicksToFinish = 1;
        Failing.Result = EStateTreeRunStatus::Failed;
        AddTransition(State("Parent"), ETrigger::OnSucceeded, ETarget::NextSibling);

        Start();
        Instance.Tick(0.1);
        EXPECT_EQ(Path(), "Next") << "the child's success completes the parent, whose transition handles it";

        EXPECT_EQ(Instance.Tick(0.2), EStateTreeRunStatus::Failed) << "a failure nothing handles fails the tree";
        EXPECT_TRUE(Instance.GetActivePath().IsEmpty());
    }

    TEST_F(FStateTreeTest, ExitStateRunsLeafToRootOnlyOnTheStatesBeingLeft)
    {
        AddState(*Tree, "StateTreeRoot");
        AddState(*Tree, "A", "StateTreeRoot");
        AddState(*Tree, "Leaf", "A");
        AddState(*Tree, "B", "StateTreeRoot");
        AddTask("StateTreeRoot", "RootTask");
        AddTask("A", "A1");
        AddTask("A", "A2");
        AddTask("Leaf", "LeafTask");
        AddTask("B", "BTask");
        AddTransition(State("Leaf"), ETrigger::OnTick, ETarget::State, "B");

        Start();
        Log.Lines.Reset();
        Instance.Tick(0.1);

        EXPECT_EQ(Path(), "StateTreeRoot/B");
        EXPECT_EQ(Lines(), "Tick RootTask, Tick A1, Tick A2, Tick LeafTask, Exit LeafTask, Exit A2, Exit A1, Enter BTask");
    }

    TEST_F(FStateTreeTest, NextSiblingParentAndFailedTargets)
    {
        AddState(*Tree, "Parent");
        AddState(*Tree, "First", "Parent");
        AddState(*Tree, "Second", "Parent");
        AddTask("Parent", "ParentTask");
        AddTask("First", "FirstTask").TicksToFinish = 1;
        AddTask("Second", "SecondTask").TicksToFinish = 1;
        AddTransition(State("First"), ETrigger::OnCompleted, ETarget::NextSibling);
        AddTransition(State("Second"), ETrigger::OnCompleted, ETarget::Parent);
        SStateTreeTransition& Abort = AddTransition(State("Parent"), ETrigger::OnEvent, ETarget::Failed);
        Abort.EventTag.TagName = FName("StateTreeTest.Abort");
        Abort.Priority = 1;

        Start();
        Instance.Tick(0.1);
        EXPECT_EQ(Path(), "Parent/Second");

        Log.Lines.Reset();
        Instance.Tick(0.2);
        EXPECT_EQ(Path(), "Parent/First") << "a transition to an active ancestor re-enters it and selects again";
        EXPECT_EQ(Lines(), "Tick ParentTask, Tick SecondTask, Exit SecondTask, Exit ParentTask, Enter ParentTask, Enter FirstTask");

        ASSERT_TRUE(Instance.SendEvent(Tag("StateTreeTest.Abort")));
        EXPECT_EQ(Instance.Tick(0.3), EStateTreeRunStatus::Failed) << "the parent's higher priority beats the leaf completing the same tick";
    }

    TEST_F(FStateTreeTest, EventTransitionsMatchTheTagOrADescendantOfIt)
    {
        AddState(*Tree, "Idle");
        AddState(*Tree, "Alerted");
        AddTask("Idle", "IdleTask");
        AddTransition(State("Idle"), ETrigger::OnEvent, ETarget::State, "Alerted").EventTag.TagName = FName("StateTreeTest.Alert");

        Start();
        ASSERT_TRUE(Instance.SendEvent(Tag("StateTreeTest.Other")));
        ASSERT_TRUE(Instance.SendEvent(Tag("StateTreeTest")));
        Instance.Tick(0.1);
        EXPECT_EQ(Path(), "Idle") << "a sibling or a parent of the transition's tag does not match";
        EXPECT_EQ(Algo::Count(Log.Lines, FString("Event IdleTask")), 2) << "every queued event reaches the active tasks";

        ASSERT_TRUE(Instance.SendEvent(Tag("StateTreeTest.Alert.Loud")));
        Instance.Tick(0.2);
        EXPECT_EQ(Path(), "Alerted");
    }

    TEST_F(FStateTreeTest, TickTransitionsWaitForTheirConditionsAndThePriorityDecides)
    {
        AddState(*Tree, "Patrol");
        AddState(*Tree, "Low");
        AddState(*Tree, "High");
        AddTask("Patrol", "PatrolTask");
        AddNode<SStateTreeTestEvaluator>(Tree->Evaluators, "Clock");

        SStateTreeTransition& ToLow = AddTransition(State("Patrol"), ETrigger::OnTick, ETarget::State, "Low");
        SStateTreeTestCondition& LowGate = AddNode<SStateTreeTestCondition>(ToLow.Conditions);
        LowGate.MinValue = 3;
        Bind(LowGate, "Value", ESource::Evaluator, "Clock", "Counter");

        SStateTreeTransition& ToHigh = AddTransition(State("Patrol"), ETrigger::OnTick, ETarget::State, "High");
        ToHigh.Priority = 1;
        SStateTreeTestCondition& HighGate = AddNode<SStateTreeTestCondition>(ToHigh.Conditions);
        HighGate.MinValue = 3;
        Bind(HighGate, "Value", ESource::Evaluator, "Clock", "Counter");

        Start();
        Instance.Tick(0.1);
        EXPECT_EQ(Path(), "Patrol") << "the evaluator has counted only Start and one tick";
        Instance.Tick(0.2);
        EXPECT_EQ(Path(), "High") << "both pass on the third evaluator tick and the higher priority wins";
    }

    TEST_F(FStateTreeTest, DelayedTransitionsFireAfterTheirDelay)
    {
        AddState(*Tree, "Wait");
        AddState(*Tree, "Done");
        AddState(*Tree, "Jitter");
        AddTask("Wait", "WaitTask");
        SStateTreeTransition& Fixed = AddTransition(State("Wait"), ETrigger::OnTick, ETarget::State, "Done");
        Fixed.Delay = 1.0f;
        SStateTreeTransition& Random = AddTransition(State("Done"), ETrigger::OnTick, ETarget::State, "Jitter");
        Random.Delay = 1.0f;
        Random.DelayRandomRange = 0.5f;

        Start();
        Instance.Tick(0.1);
        EXPECT_DOUBLE_EQ(Instance.GetNextWakeTime(), 1.1);
        Instance.Tick(0.9);
        EXPECT_EQ(Path(), "Wait") << "an OnTick trigger every frame must not restart the pending delay";
        Instance.Tick(1.15);
        EXPECT_EQ(Path(), "Done");

        Instance.Tick(1.2);
        EXPECT_GE(Instance.GetNextWakeTime(), 1.7);
        EXPECT_LE(Instance.GetNextWakeTime(), 2.7);
        Instance.Tick(1.6);
        EXPECT_EQ(Path(), "Done") << "the shortest drawn delay is 0.5";
        Instance.Tick(2.8);
        EXPECT_EQ(Path(), "Jitter");
    }

    TEST_F(FStateTreeTest, BindingsCopyParametersEvaluatorsEarlierTasksAndTheContext)
    {
        Tree->Parameters.InitializeAs<SStateTreeTestParameters>().Speed = 3;
        AddNode<SStateTreeTestEvaluator>(Tree->Evaluators, "Clock");
        AddState(*Tree, "Parent");
        AddState(*Tree, "Child", "Parent");

        SStateTreeTestTask& FromParameter = AddTask("Parent", "FromParameter");
        FromParameter.OutputOffset = 100;
        Bind(FromParameter, "Input", ESource::Parameter, "", "Speed");
        Bind(FromParameter, "Owner", ESource::Context, "", "Owner");
        Bind(FromParameter, "DeltaTime", ESource::Context, "", "DeltaTime");

        Bind(AddTask("Parent", "FromEvaluator"), "Input", ESource::Evaluator, "Clock", "Counter");
        Bind(AddTask("Child", "FromTask"), "Input", ESource::Task, "FromParameter", "Output");

        TInstancedStruct<SStateTreeParametersBase> Override;
        Override.InitializeAs<SStateTreeTestParameters>().Speed = 42;
        Start(0.0, &Override);

        EXPECT_EQ(TaskData("Parent", 0).Input, 42) << "the instance's parameters replace the tree's defaults";
        EXPECT_EQ(TaskData("Parent", 0).Output, 142);
        EXPECT_EQ(TaskData("Parent", 0).Owner, ECS::FEntity(5, 1));
        EXPECT_EQ(TaskData("Parent", 1).Input, 1) << "the evaluator ticks once before selection";
        EXPECT_EQ(TaskData("Child").Input, 142) << "a child task reads its parent task's output";

        Instance.Tick(0.25);
        EXPECT_FLOAT_EQ(TaskData("Parent", 0).DeltaTime, 0.25f);
        EXPECT_EQ(TaskData("Parent", 1).Input, 2);
    }

    TEST_F(FStateTreeTest, FunctionBindingsDeliverTheirReturnValueEveryTickWithoutAllocating)
    {
        Tree->Parameters.InitializeAs<SStateTreeTestParameters>().Speed = 3;
        AddNode<SStateTreeTestEvaluator>(Tree->Evaluators, "Clock");
        AddState(*Tree, "Only");
        // Tasks without a log, so a tick allocates nothing of the test's own.
        SStateTreeBinding& Sum = Bind(AddNode<SStateTreeTestTask>(State("Only").Tasks, "Sum"), "Input", ESource::Function, "CStateTreeTestLibrary", "AddInts");
        AddArgument(Sum, ESource::Parameter, "", "Speed");
        AddArgument(Sum, ESource::Evaluator, "Clock", "Counter");
        Bind(AddNode<SStateTreeTestTask>(State("Only").Tasks, "Getter"), "Input", ESource::Evaluator, "Clock", "GetDoubledCounter");

        Start();
        EXPECT_EQ(TaskData("Only", 0).Input, 4) << "AddInts(Speed, Counter) with the evaluator ticked once";
        EXPECT_EQ(TaskData("Only", 1).Input, 2) << "the getter runs on the evaluator's instance data";

        const uint32 Category = Memory::RegisterCategory("StateTreeFunctionBindingTicks");
        auto TotalAllocations = [Category]
        {
            TVector<Memory::FMemoryCategoryStats> Stats(Category + 1, {});
            Memory::GetCategoryStats(Stats.GetData(), Category + 1);
            return Stats[Category].TotalAllocs;
        };
        const uint64 AllocationsBefore = TotalAllocations();
        for (int32 Tick = 1; Tick <= 3; ++Tick)
        {
            {
                Memory::FMemoryScope Scope(Category);
                Instance.Tick(Tick * 0.1);
            }
            EXPECT_EQ(TaskData("Only", 0).Input, 4 + Tick);
            EXPECT_EQ(TaskData("Only", 1).Input, 2 * (1 + Tick));
        }
        EXPECT_EQ(TotalAllocations(), AllocationsBefore) << "a tick calls into the preallocated frame and copies, nothing more";
    }

    TEST_F(FStateTreeTest, AWaitingInstanceIsNotTickedUntilItsWakeTimeOrAnEvent)
    {
        AddState(*Tree, "Sleep");
        AddState(*Tree, "Woken");
        AddState(*Tree, "Poked");
        SStateTreeTestTask& Sleeper = AddTask("Sleep", "Sleeper");
        Sleeper.bNeedsTick = false;
        Sleeper.WakeAfter = 2.0f;
        AddTransition(State("Sleep"), ETrigger::OnSucceeded, ETarget::State, "Woken");
        AddTransition(State("Sleep"), ETrigger::OnEvent, ETarget::State, "Poked").EventTag.TagName = FName("StateTreeTest.Poke");

        Start();
        EXPECT_TRUE(Instance.IsWaiting());
        EXPECT_DOUBLE_EQ(Instance.GetNextWakeTime(), 2.0);
        Instance.Tick(1.0);
        Instance.Tick(1.9);
        EXPECT_EQ(TaskData("Sleep").TickCount, 0);
        Instance.Tick(2.0);
        EXPECT_EQ(Path(), "Woken") << "the wake time ticks it once and the task completes";

        FStateTreeInstance Second;
        Second.Start(Tree->GetRuntime(), nullptr, ECS::NullEntity, 0.0);
        Second.Tick(0.5);
        const uint16 SleeperNode = Tree->GetRuntime()->States[FindState(*Tree->GetRuntime(), "Sleep")].FirstTask;
        EXPECT_EQ(static_cast<const SStateTreeTestTaskData*>(Second.GetNodeInstanceData(SleeperNode))->TickCount, 0);
        for (int32 Index = 0; Index < kStateTreeMaxEvents; ++Index)
        {
            ASSERT_TRUE(Second.SendEvent(Tag("StateTreeTest.Poke")));
        }
        EXPECT_FALSE(Second.SendEvent(Tag("StateTreeTest.Poke"))) << "the queue has a fixed capacity";
        EXPECT_FALSE(Second.IsWaiting());
        Second.Tick(0.6);
        EXPECT_EQ(PathOf(Second, *Tree->GetRuntime()), "Poked");
    }

    TEST_F(FStateTreeTest, RecompilingStopsRunningInstancesOnTheLayoutTheyStartedWith)
    {
        AddState(*Tree, "Work");
        AddTask("Work", "WorkTask");
        Start();
        FStateTreeInstance Second;
        Second.Start(Tree->GetRuntime(), nullptr, ECS::NullEntity, 0.0);
        Log.Lines.Reset();

        AddState(*Tree, "Other");
        AddTask("Other", "OtherTask");
        AddTask("Work", "AddedTask");
        ASSERT_TRUE(Tree->Compile());

        EXPECT_EQ(Instance.Tick(1.0), EStateTreeRunStatus::Failed);
        EXPECT_EQ(Lines(), "Exit WorkTask") << "only the task of the old compile exits, from the old block";
        Second.Stop(1.0);
        EXPECT_EQ(Lines(), "Exit WorkTask, Exit WorkTask");

        Log.Lines.Reset();
        Start(2.0);
        EXPECT_EQ(Path(), "Work");
        EXPECT_EQ(Lines(), "Enter WorkTask, Enter AddedTask");
    }

    TEST_F(FStateTreeTest, ALinkedSubtreeRunsInsideTheParentAndReceivesItsParameters)
    {
        CStateTree* Sub = NewTree();
        Sub->Parameters.InitializeAs<SStateTreeTestParameters>().Speed = 1;
        AddState(*Sub, "SubtreeWork");
        SStateTreeTestTask& Work = AddTask("SubtreeWork", "SubTask", Sub);
        Work.TicksToFinish = 1;
        Bind(Work, "Input", ESource::Parameter, "", "Speed");

        Tree->Parameters.InitializeAs<SStateTreeTestParameters>().Speed = 7;
        AddState(*Tree, "LiteralLink", nullptr, EKind::LinkedSubtree);
        AddState(*Tree, "BoundLink", nullptr, EKind::LinkedSubtree);
        AddState(*Tree, "AfterLinks");
        AddTask("AfterLinks", "DoneTask");
        State("LiteralLink").LinkedSubtree = Sub;
        State("LiteralLink").LinkedParameters.InitializeAs<SStateTreeTestParameters>().Speed = 5;
        AddTransition(State("LiteralLink"), ETrigger::OnSucceeded, ETarget::NextSibling);
        State("BoundLink").LinkedSubtree = Sub;
        Bind(State("BoundLink").ParameterBindings, "Speed", ESource::Parameter, "", "Speed");
        AddTransition(State("BoundLink"), ETrigger::OnSucceeded, ETarget::State, "AfterLinks");

        Start();
        const FStateTreeRuntime& Runtime = *Tree->GetRuntime();
        for (const FStateTreeNode& Node : Runtime.Nodes)
        {
            EXPECT_EQ(Node.InstanceDataOffset % Node.InstanceDataType->GetAlignment(), 0u);
            EXPECT_LE(Node.InstanceDataOffset + Node.InstanceDataType->GetSize(), Runtime.InstanceDataSize) << "the parent's one block holds the subtree's task data";
            EXPECT_GE(Runtime.InstanceDataAlignment, Node.InstanceDataType->GetAlignment());
        }
        ASSERT_EQ(Runtime.Parameters.Num(), 3) << "the root tree and each link keep their own parameters";
        EXPECT_LE(Runtime.Parameters[2].Offset + SStateTreeTestParameters::StaticStruct()->GetSize(), Runtime.InstanceDataSize);

        EXPECT_EQ(Path(), "LiteralLink/SubtreeWork");
        EXPECT_EQ(LeafTaskData().Input, 5) << "the linking state's literal replaces the subtree's default";

        Instance.Tick(0.1);
        EXPECT_EQ(Path(), "BoundLink/SubtreeWork") << "the subtree succeeding fires the linking state's OnSucceeded";
        EXPECT_EQ(LeafTaskData().Input, 7) << "a parameter binding copies the parent's parameter into the subtree's";

        Instance.Tick(0.2);
        EXPECT_EQ(Path(), "AfterLinks");
        EXPECT_EQ(Lines(), "Enter SubTask, Tick SubTask, Exit SubTask, Enter SubTask, Tick SubTask, Exit SubTask, Enter DoneTask");
    }

    TEST_F(FStateTreeTest, ALinkedSubtreeFailingFiresTheLinkingStatesFailedTransition)
    {
        CStateTree* Sub = NewTree();
        AddState(*Sub, "SubtreeTry");
        AddTask("SubtreeTry", "TryTask", Sub).TicksToFinish = 1;
        AddTransition(State("SubtreeTry", Sub), ETrigger::OnSucceeded, ETarget::Failed);

        AddState(*Tree, "FailingLink", nullptr, EKind::LinkedSubtree);
        AddState(*Tree, "Wrong");
        AddState(*Tree, "Recovered");
        State("FailingLink").LinkedSubtree = Sub;
        AddTransition(State("FailingLink"), ETrigger::OnSucceeded, ETarget::State, "Wrong");
        AddTransition(State("FailingLink"), ETrigger::OnFailed, ETarget::State, "Recovered");

        Start();
        EXPECT_EQ(Path(), "FailingLink/SubtreeTry");
        EXPECT_EQ(Instance.Tick(0.1), EStateTreeRunStatus::Running) << "the subtree's Failed target ends the subtree, not the whole tree";
        EXPECT_EQ(Path(), "FailingLink");
        EXPECT_FALSE(Instance.IsWaiting());
        Instance.Tick(0.2);
        EXPECT_EQ(Path(), "Recovered");
    }
}
