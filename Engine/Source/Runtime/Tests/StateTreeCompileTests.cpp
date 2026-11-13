#include <gtest/gtest.h>

#include "AI/StateTree/StateTreeTestTypes.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Serialization/MemoryArchiver.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaStateTreeCompileTests
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

    bool HasError(const FStateTreeCompiler& Compiler, const char* Fragment)
    {
        return Algo::AnyOf(Compiler.GetErrors(), [Fragment](const FString& Error) { return Error.Contains(Fragment); });
    }

    uint32 FieldOffset(CStruct* Struct, const char* Field)
    {
        return Struct->GetProperty(FName(Field))->Offset;
    }

    TEST(StateTreeCompile, AMissingTransitionTargetIsACompileError)
    {
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Only");
        AddTransition(Tree->States[0], ETrigger::OnCompleted, ETarget::State, "Nowhere");
        AddTransition(Tree->States[0], ETrigger::OnCompleted, ETarget::NextSibling);

        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        EXPECT_FALSE(Compiler.Compile(*Tree, Runtime));
        EXPECT_TRUE(HasError(Compiler, "missing state 'Nowhere'"));
        EXPECT_TRUE(HasError(Compiler, "missing next sibling"));
        EXPECT_FALSE(Runtime.IsValid()) << "a failed compile leaves nothing to run";
    }

    TEST(StateTreeCompile, ATypeMismatchedBindingIsACompileError)
    {
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Only");
        Bind(AddNode<SStateTreeTestTask>(Tree->States[0].Tasks, "Task"), "Input", ESource::Context, "", "DeltaTime");

        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        EXPECT_FALSE(Compiler.Compile(*Tree, Runtime));
        EXPECT_TRUE(HasError(Compiler, "binds 'Input' of type"));
    }

    TEST(StateTreeCompile, FunctionBindingMistakesAreCompileErrors)
    {
        CStateTree* Tree = NewTree();
        Tree->Parameters.InitializeAs(SStateTreeTestParameters::StaticStruct());
        AddState(*Tree, "Only");
        SStateTreeTestTask& Task = AddNode<SStateTreeTestTask>(Tree->States[0].Tasks, "Task");
        Bind(Task, "Input", ESource::Function, "CStateTreeTestLibrary", "Missing");
        Bind(Task, "Input", ESource::Function, "MissingLibrary", "AddInts");
        AddArgument(Bind(Task, "DeltaTime", ESource::Function, "CStateTreeTestLibrary", "AddInts"), ESource::Parameter, "", "Speed");
        AddArgument(Bind(Task, "Input", ESource::Function, "CStateTreeTestLibrary", "AddInts"), ESource::Parameter, "", "Speed");
        SStateTreeBinding& WrongArgument = Bind(Task, "Output", ESource::Function, "CStateTreeTestLibrary", "AddInts");
        AddArgument(WrongArgument, ESource::Parameter, "", "Speed");
        AddArgument(WrongArgument, ESource::Context, "", "DeltaTime");
        Bind(Task, "Input", ESource::Function, "CStateTreeTestLibrary", "ReturnsNothing");

        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        EXPECT_FALSE(Compiler.Compile(*Tree, Runtime));
        EXPECT_TRUE(HasError(Compiler, "unknown function 'CStateTreeTestLibrary.Missing'"));
        EXPECT_TRUE(HasError(Compiler, "unknown function 'MissingLibrary.AddInts'"));
        EXPECT_TRUE(HasError(Compiler, "binds 'DeltaTime' of type")) << "the return type must match the destination field";
        EXPECT_TRUE(HasError(Compiler, "calls 'AddInts' with 1 arguments, and it takes 2"));
        EXPECT_TRUE(HasError(Compiler, "passes 'DeltaTime' of type"));
        EXPECT_TRUE(HasError(Compiler, "'ReturnsNothing', which returns nothing"));
        EXPECT_EQ(Compiler.GetErrors().Num(), 6);
    }

    TEST(StateTreeCompile, AFunctionBindingResolvesToACallWithAFrameInTheBlock)
    {
        CStateTree* Tree = NewTree();
        Tree->Parameters.InitializeAs(SStateTreeTestParameters::StaticStruct());
        AddNode<SStateTreeTestEvaluator>(Tree->Evaluators, "Clock");
        AddState(*Tree, "Only");
        SStateTreeBinding& Sum = Bind(AddNode<SStateTreeTestTask>(Tree->States[0].Tasks, "Task"), "Input", ESource::Function, "CStateTreeTestLibrary", "AddInts");
        AddArgument(Sum, ESource::Parameter, "", "Speed");
        AddArgument(Sum, ESource::Evaluator, "Clock", "Counter");

        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        ASSERT_TRUE(Compiler.Compile(*Tree, Runtime));

        ASSERT_EQ(Runtime.Calls.Num(), 1);
        const FStateTreeCall& Call = Runtime.Calls[0];
        const FFunction* AddInts = CStateTreeTestLibrary::StaticClass()->FindFunction(FName("AddInts"));
        EXPECT_EQ(Call.Function, AddInts);
        EXPECT_EQ(Call.FrameOffset % FFunction::kFrameAlignment, 0u);
        EXPECT_LE(Call.FrameOffset + AddInts->GetParmsSize(), Runtime.InstanceDataSize);
        ASSERT_EQ(Call.NumArguments, 2);
        EXPECT_EQ(Runtime.Copies[Call.FirstArgument].SrcOffset, Runtime.Parameters[0].Offset + FieldOffset(SStateTreeTestParameters::StaticStruct(), "Speed"));
        EXPECT_EQ(Runtime.Copies[Call.FirstArgument + 1].SrcOffset, Runtime.Nodes[0].InstanceDataOffset + FieldOffset(SStateTreeTestEvaluatorData::StaticStruct(), "Counter"));

        const FStateTreeNode& Node = Runtime.Nodes[1];
        ASSERT_EQ(Node.NumCopies, 1) << "the argument copies sit outside the node's batch";
        const FStateTreeCopy& Result = Runtime.Copies[Node.FirstCopy];
        EXPECT_EQ(Result.Call, 0);
        EXPECT_EQ(Result.SrcOffset, Call.FrameOffset + AddInts->GetReturnParam()->Offset);
        EXPECT_EQ(Result.DstOffset, Node.InstanceDataOffset + FieldOffset(SStateTreeTestTaskData::StaticStruct(), "Input"));
    }

    TEST(StateTreeCompile, StructuralMistakesAreCollectedRatherThanAsserted)
    {
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Twice");
        AddState(*Tree, "Twice");
        AddState(*Tree, "Linked", nullptr, EStateTreeStateKind::LinkedSubtree);
        AddState(*Tree, "Orphan", "Missing");

        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        EXPECT_FALSE(Compiler.Compile(*Tree, Runtime));
        EXPECT_TRUE(HasError(Compiler, "'Twice' is used twice"));
        EXPECT_TRUE(HasError(Compiler, "'Linked' is a linked subtree with no tree to link"));
        EXPECT_TRUE(HasError(Compiler, "missing parent 'Missing'"));
    }

    TEST(StateTreeCompile, ALoopingParentChainIsACompileError)
    {
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Top");
        AddState(*Tree, "Loop", "Back");
        AddState(*Tree, "Back", "Loop");

        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        EXPECT_FALSE(Compiler.Compile(*Tree, Runtime));
        EXPECT_TRUE(HasError(Compiler, "parent chain loops"));
    }

    TEST(StateTreeCompile, ATreeLinkingItselfIsACompileError)
    {
        CStateTree* Tree = NewTree();
        AddState(*Tree, "SelfLink", nullptr, EStateTreeStateKind::LinkedSubtree);
        Tree->States[0].LinkedSubtree = Tree;

        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        EXPECT_FALSE(Compiler.Compile(*Tree, Runtime));
        EXPECT_TRUE(HasError(Compiler, "'SelfLink' links"));
        EXPECT_TRUE(HasError(Compiler, "in a cycle"));
        Tree->States[0].LinkedSubtree = nullptr;
    }

    TEST(StateTreeCompile, TreesLinkingEachOtherAreACompileErrorFromEitherSide)
    {
        CStateTree* First = NewTree();
        CStateTree* Second = NewTree();
        AddState(*First, "Start");
        AddState(*First, "ToSecond", nullptr, EStateTreeStateKind::LinkedSubtree);
        First->States[1].LinkedSubtree = Second;
        AddState(*Second, "ToFirst", nullptr, EStateTreeStateKind::LinkedSubtree);
        Second->States[0].LinkedSubtree = First;

        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        EXPECT_FALSE(Compiler.Compile(*First, Runtime));
        EXPECT_TRUE(HasError(Compiler, "'ToFirst' links"));
        EXPECT_TRUE(HasError(Compiler, "in a cycle"));
        EXPECT_FALSE(Compiler.Compile(*Second, Runtime));
        EXPECT_TRUE(HasError(Compiler, "'ToSecond' links"));
        First->States[1].LinkedSubtree = nullptr;
        Second->States[0].LinkedSubtree = nullptr;
    }

    TEST(StateTreeCompile, LinkingOneTreeTwiceIsNotACycle)
    {
        CStateTree* Shared = NewTree();
        AddState(*Shared, "Leaf");
        AddTransition(Shared->States[0], ETrigger::OnCompleted, ETarget::State, "Leaf");
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Leaf");
        AddState(*Tree, "Left", nullptr, EStateTreeStateKind::LinkedSubtree);
        AddState(*Tree, "Right", nullptr, EStateTreeStateKind::LinkedSubtree);
        Tree->States[1].LinkedSubtree = Shared;
        Tree->States[2].LinkedSubtree = Shared;

        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        ASSERT_TRUE(Compiler.Compile(*Tree, Runtime));
        ASSERT_EQ(Runtime.States.Num(), 5);
        for (const FStateTreeRuntimeTransition& Transition : Runtime.Transitions)
        {
            const FStateTreeRuntimeState& Target = Runtime.States[Transition.TargetState];
            EXPECT_EQ(Target.Depth, 1) << "a linked tree's transition targets its own copy of 'Leaf', not the parent's";
            EXPECT_EQ(Runtime.States[Target.Parent].FirstChild, Transition.TargetState);
        }
        EXPECT_NE(Runtime.Transitions[0].TargetState, Runtime.Transitions[1].TargetState);
    }

    TEST(StateTreeCompile, ATaskCannotBindToALaterTaskOrAnotherBranch)
    {
        CStateTree* Tree = NewTree();
        AddState(*Tree, "Left");
        AddState(*Tree, "Right");
        Bind(AddNode<SStateTreeTestTask>(Tree->States[0].Tasks, "Early"), "Input", ESource::Task, "Late", "Output");
        AddNode<SStateTreeTestTask>(Tree->States[0].Tasks, "Late");
        Bind(AddNode<SStateTreeTestTask>(Tree->States[1].Tasks, "Elsewhere"), "Input", ESource::Task, "Late", "Output");

        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        EXPECT_FALSE(Compiler.Compile(*Tree, Runtime));
        EXPECT_EQ(Compiler.GetErrors().Num(), 2);
    }

    TEST(StateTreeCompile, BindingsBecomeByteCopiesInOneAlignedBlock)
    {
        CStateTree* Tree = NewTree();
        Tree->Parameters.InitializeAs(SStateTreeTestParameters::StaticStruct());
        AddNode<SStateTreeTestEvaluator>(Tree->Evaluators, "Clock");
        AddState(*Tree, "Only");
        SStateTreeTestTask& Task = AddNode<SStateTreeTestTask>(Tree->States[0].Tasks, "Task");
        Bind(Task, "Input", ESource::Parameter, "", "Speed");
        Bind(Task, "Owner", ESource::Context, "", "Owner");

        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        ASSERT_TRUE(Compiler.Compile(*Tree, Runtime));

        ASSERT_EQ(Runtime.Nodes.Num(), 2);
        const FStateTreeNode& Node = Runtime.Nodes[1];
        CStruct* TaskData = SStateTreeTestTaskData::StaticStruct();
        EXPECT_EQ(Node.InstanceDataType, TaskData);
        EXPECT_EQ(Node.InstanceDataOffset % TaskData->GetAlignment(), 0u);
        EXPECT_EQ(Runtime.Parameters[0].Offset % SStateTreeTestParameters::StaticStruct()->GetAlignment(), 0u);
        EXPECT_GE(Runtime.InstanceDataSize, Node.InstanceDataOffset + TaskData->GetSize());
        EXPECT_EQ(Runtime.InstanceDataAlignment, alignof(SStateTreeTestTaskData));

        ASSERT_EQ(Node.NumCopies, 2);
        const FStateTreeCopy& FromParameter = Runtime.Copies[Node.FirstCopy];
        EXPECT_EQ(FromParameter.SrcOffset, Runtime.Parameters[0].Offset + FieldOffset(SStateTreeTestParameters::StaticStruct(), "Speed"));
        EXPECT_EQ(FromParameter.DstOffset, Node.InstanceDataOffset + FieldOffset(TaskData, "Input"));
        EXPECT_EQ(FromParameter.Size, sizeof(int32));
        const FStateTreeCopy& FromContext = Runtime.Copies[Node.FirstCopy + 1];
        EXPECT_EQ(FromContext.SrcOffset, FieldOffset(SStateTreeContextData::StaticStruct(), "Owner"));
        EXPECT_EQ(FromContext.Size, sizeof(ECS::FEntity));
    }

    TEST(StateTreeCompile, ASavedTreeLoadsRecompilesAndRunsTheSame)
    {
        CStateTree* Tree = NewTree();
        Tree->Parameters.InitializeAs<SStateTreeTestParameters>().Speed = 9;
        AddState(*Tree, "First");
        AddState(*Tree, "Second");
        SStateTreeTestTask& Task = AddNode<SStateTreeTestTask>(Tree->States[0].Tasks, "Task");
        Task.TicksToFinish = 1;
        Bind(Task, "Input", ESource::Parameter, "", "Speed");
        SStateTreeTransition& Transition = AddTransition(Tree->States[0], ETrigger::OnEvent, ETarget::NextSibling);
        Transition.EventTag.TagName = FName("StateTreeTest.Go");
        Transition.Delay = 0.5f;

        TVector<uint8> Bytes;
        {
            FMemoryWriter Writer(Bytes);
            Tree->Serialize(Writer);
        }
        CStateTree* Loaded = NewTree();
        Loaded->Version = 0;
        FMemoryReader Reader(Bytes);
        Loaded->Serialize(Reader);
        Loaded->PostLoad();

        EXPECT_EQ(Loaded->Version, kStateTreeVersion);
        ASSERT_TRUE(Loaded->GetRuntime()->IsValid()) << "PostLoad compiles the loaded tree";
        ASSERT_EQ(Loaded->GetRuntime()->States.Num(), 2);
        const FStateTreeRuntimeTransition& Loaded0 = Loaded->GetRuntime()->Transitions[0];
        EXPECT_EQ(Loaded0.Trigger, ETrigger::OnEvent);
        EXPECT_EQ(Loaded0.TargetState, 1);
        EXPECT_FLOAT_EQ(Loaded0.Delay, 0.5f);
        EXPECT_EQ(Loaded->GetRuntime()->Copies.Num(), 1);

        FStateTreeInstance Instance;
        Instance.Start(Loaded->GetRuntime(), nullptr, ECS::NullEntity, 0.0);
        const SStateTreeTestTaskData& Data = *static_cast<const SStateTreeTestTaskData*>(Instance.GetNodeInstanceData(Loaded->GetRuntime()->States[0].FirstTask));
        EXPECT_EQ(Data.Input, 9) << "the parameter defaults survived the round trip";

        Tree->Version = kStateTreeVersion + 1;
        TVector<uint8> NewerBytes;
        {
            FMemoryWriter Writer(NewerBytes);
            Tree->Serialize(Writer);
        }
        CStateTree* Newer = NewTree();
        FMemoryReader NewerReader(NewerBytes);
        Newer->Serialize(NewerReader);
        EXPECT_EQ(Newer->Version, kStateTreeVersion + 1);
        FStateTreeCompiler Compiler;
        FStateTreeRuntime Runtime;
        EXPECT_FALSE(Compiler.Compile(*Newer, Runtime));
        EXPECT_TRUE(HasError(Compiler, "newer than this build"));
    }

    TEST(StateTreeCompile, EditingALinkedSubtreeRecompilesEveryTreeLinkingIt)
    {
        CStateTree* Sub = NewTree();
        AddState(*Sub, "Leaf");
        CStateTree* Middle = NewTree();
        AddState(*Middle, "ToSub", nullptr, EStateTreeStateKind::LinkedSubtree);
        Middle->States[0].LinkedSubtree = Sub;
        CStateTree* Top = NewTree();
        AddState(*Top, "ToMiddle", nullptr, EStateTreeStateKind::LinkedSubtree);
        Top->States[0].LinkedSubtree = Middle;
        ASSERT_TRUE(Top->Compile());
        ASSERT_EQ(Top->GetRuntime()->States.Num(), 3);
        TSharedPtr<const FStateTreeRuntime> Running = Top->GetRuntime();

        AddState(*Sub, "Added");
        Sub->PostPropertyChange(nullptr);

        EXPECT_EQ(Middle->GetRuntime()->States.Num(), 3) << "the direct linker took the edit";
        EXPECT_EQ(Top->GetRuntime()->States.Num(), 4) << "a tree linking through another one took the edit";
        EXPECT_TRUE(Running->bSuperseded);
    }

    TEST(StateTreeCompile, AParentLoadedBeforeItsSubtreeCompilesAgainstTheLoadedSubtree)
    {
        CStateTree* Saved = NewTree();
        AddState(*Saved, "Leaf");
        TVector<uint8> Bytes;
        {
            FMemoryWriter Writer(Bytes);
            Saved->Serialize(Writer);
        }

        CStateTree* Sub = NewTree();
        CStateTree* Parent = NewTree();
        AddState(*Parent, "ToSub", nullptr, EStateTreeStateKind::LinkedSubtree);
        Parent->States[0].LinkedSubtree = Sub;
        Parent->PostLoad();
        EXPECT_FALSE(Parent->GetRuntime()->IsValid()) << "the subtree has no states yet";

        FMemoryReader Reader(Bytes);
        Sub->Serialize(Reader);
        Sub->PostLoad();

        ASSERT_TRUE(Parent->GetRuntime()->IsValid());
        EXPECT_EQ(Parent->GetRuntime()->States.Num(), 2);
    }
}
