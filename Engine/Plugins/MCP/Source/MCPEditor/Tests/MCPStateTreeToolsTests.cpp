#include "MCPStateTreeTools.h"
#include "MCPToolTestUtil.h"
#include "ScopedTempMountTestUtil.h"

#include "AI/StateTree/StateTreeTestTypes.h"
#include "Agent/AgentAssetResolve.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/Factories/Factory.h"
#include "Core/Object/ObjectCore.h"
#include "FileSystem/FileSystem.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPStateTreeToolsTestsInternal
{
    class MCPStateTreeToolsTest : public FRegisteredToolsTest
    {
    protected:

        MCPStateTreeToolsTest() : FRegisteredToolsTest("MCPStateTreeToolsTests", &RegisterStateTreeTools) {}

        void SetUp() override
        {
            RegisterPendingTypes();
            FRegisteredToolsTest::SetUp();
            ASSERT_TRUE(Filesystem::MakeDirectoryTree(Mount.Dir + "/Content/AI"));

            const FJsonValue Created = ExpectSucceeded("statetree.create",
                FStringView(Format(R"({{"Folder":"/Game/Content/AI","Name":"{}"}})", testing::UnitTest::GetInstance()->current_test_info()->name())));
            Guid = FString(Created["structuredContent"]["Guid"].AsString());

            // Held as an open editor tab would, since the server that keeps edited assets loaded is not running here.
            CStateTree* Loaded = nullptr;
            FString Error;
            ASSERT_TRUE(Agent::ResolveAsset(FStringView(Guid), Loaded, Error)) << *Error;
            Tree = TStrongObjectPtr<CStateTree>(Loaded);
        }

        void TearDown() override
        {
            if (Tree)
            {
                FAssetRegistry::Get().AssetDeleted(Tree->GetGUID());
                CFactory::DiscardNew(Tree.Get());
                Tree.Reset();
            }
            FRegisteredToolsTest::TearDown();
        }

        FJsonValue Edit(const char* Tool, const char* Fields)
        {
            return ExpectSucceeded(FStringView(Tool), FStringView(Format(R"({{"StateTree":"{}",{}}})", Guid, Fields)));
        }

        void ExpectEditRefused(const char* Tool, const char* Fields, const char* Expected)
        {
            ExpectRefused(FStringView(Tool), FStringView(Format(R"({{"StateTree":"{}",{}}})", Guid, Fields)), Expected);
        }

        FJsonValue Compile()
        {
            return FJsonValue(ExpectSucceeded("statetree.compile", FStringView(Format(R"({{"StateTree":"{}"}})", Guid)))["structuredContent"]);
        }

        TestUtil::FScopedTempMount Mount{ "/Game", "LuminaMCPStateTreeToolsTests" };
        TStrongObjectPtr<CStateTree> Tree;
        FString Guid;
    };
}

using namespace MCPStateTreeToolsTestsInternal;

TEST_F(MCPStateTreeToolsTest, BuildsTwoStatesWithATaskBindingAndTransitionThatCompile)
{
    Edit("statetree.add_state", R"("State":{"Name":"Idle"})");
    Edit("statetree.add_state", R"("State":{"Name":"Run"})");
    Edit("statetree.add_node", R"("Type":"SStateTreeTestEvaluator","Name":"Counter")");
    Edit("statetree.add_node", R"("Type":"SStateTreeTestTask","Name":"Wait","State":"Idle","Properties":{"TicksToFinish":2})");
    Edit("statetree.set_binding", R"("Node":"Wait","Binding":{"TargetField":"Input","Source":"Evaluator","SourceNode":"Counter","SourceField":"Counter"})");
    const FJsonValue Transition = Edit("statetree.add_transition", R"("State":"Idle","Transition":{"Trigger":"OnCompleted","Target":"State","TargetState":"Run"})");
    EXPECT_EQ(Transition["structuredContent"]["Index"].AsInt64(), 0);

    const FJsonValue Report = Compile();
    EXPECT_TRUE(Report["bCompiled"].AsBool(false));
    EXPECT_EQ(Report["Errors"].Num(), 0);

    ASSERT_EQ(Tree->States.Num(), 2);
    const SStateTreeState& Idle = Tree->States[0];
    ASSERT_EQ(Idle.Tasks.Num(), 1);
    const SStateTreeTestTask* Wait = Idle.Tasks[0].GetPtr<SStateTreeTestTask>();
    ASSERT_NE(Wait, nullptr);
    EXPECT_EQ(Wait->Name, FName("Wait"));
    EXPECT_EQ(Wait->TicksToFinish, 2);
    ASSERT_EQ(Wait->Bindings.Num(), 1);
    EXPECT_EQ(Wait->Bindings[0].Source, EStateTreeBindingSource::Evaluator);
    ASSERT_EQ(Idle.Transitions.Num(), 1);
    EXPECT_EQ(Idle.Transitions[0].TargetState, FName("Run"));

    const TSharedPtr<const FStateTreeRuntime> Runtime = Tree->GetRuntime();
    ASSERT_TRUE(Runtime->IsValid());
    EXPECT_EQ(Runtime->States.Num(), 2);
    EXPECT_EQ(Runtime->Copies.Num(), 1);
    EXPECT_EQ(Runtime->NumEvaluators, 1);
}

TEST_F(MCPStateTreeToolsTest, CompileReportsEveryErrorOfAnInvalidTree)
{
    Edit("statetree.add_state", R"("State":{"Name":"Idle"})");
    Edit("statetree.add_node", R"("Type":"SStateTreeTestTask","Name":"Wait","State":"Idle")");
    Edit("statetree.set_binding", R"("Node":"Wait","Binding":{"TargetField":"Missing","Source":"Parameter","SourceField":"Speed"})");
    Edit("statetree.add_transition", R"("State":"Idle","Transition":{"Target":"State","TargetState":"Nowhere"})");

    const FJsonValue Report = Compile();
    EXPECT_FALSE(Report["bCompiled"].AsBool(true));
    ASSERT_EQ(Report["Errors"].Num(), 2);

    const FString TransitionError(Report["Errors"][0].AsString());
    const FString BindingError(Report["Errors"][1].AsString());
    EXPECT_NE(TransitionError.Find("state 'Nowhere'"), Constants::kIndexNone) << *TransitionError;
    EXPECT_NE(BindingError.Find("missing field 'Missing'"), Constants::kIndexNone) << *BindingError;
    EXPECT_FALSE(Tree->GetRuntime()->IsValid());

    const FJsonValue Described = ExpectSucceeded("statetree.describe", FStringView(Format(R"({{"StateTree":"{}"}})", Guid)));
    EXPECT_EQ(Described["structuredContent"]["Errors"].Num(), 2);
    EXPECT_NE(FString(Described["structuredContent"]["Tree"].AsString()).Find("Nowhere"), Constants::kIndexNone);
}

TEST_F(MCPStateTreeToolsTest, SetBindingReplacesThenRemovesTheBindingOfOneField)
{
    Edit("statetree.add_state", R"("State":{"Name":"Idle"})");
    Edit("statetree.add_node", R"("Type":"SStateTreeTestTask","Name":"Wait","State":"Idle")");
    Edit("statetree.set_binding", R"("Node":"Wait","Binding":{"TargetField":"Input","Source":"Parameter","SourceField":"Speed"})");
    Edit("statetree.set_binding", R"("Node":"Wait","Binding":{"TargetField":"Input","Source":"Context","SourceField":"DeltaTime"})");

    const SStateTreeTestTask* Wait = Tree->States[0].Tasks[0].GetPtr<SStateTreeTestTask>();
    ASSERT_EQ(Wait->Bindings.Num(), 1);
    EXPECT_EQ(Wait->Bindings[0].Source, EStateTreeBindingSource::Context);

    Edit("statetree.set_binding", R"("Node":"Wait","Binding":{"TargetField":"Input"})");
    EXPECT_TRUE(Wait->Bindings.IsEmpty());
    ExpectEditRefused("statetree.set_binding", R"("Node":"Wait","Binding":{"TargetField":"Input"})", "no binding to 'Input'");
}

TEST_F(MCPStateTreeToolsTest, ConditionsGoOnEnterOrOnTheNamedTransition)
{
    Edit("statetree.add_state", R"("State":{"Name":"Idle"})");
    Edit("statetree.add_transition", R"("State":"Idle","Transition":{"Target":"Succeeded"})");
    Edit("statetree.add_node", R"("Type":"SStateTreeTestCondition","Name":"Gate","State":"Idle")");
    Edit("statetree.add_node", R"("Type":"SStateTreeTestCondition","Name":"Guard","State":"Idle","Transition":0,"Properties":{"bInvert":true})");

    const SStateTreeState& Idle = Tree->States[0];
    ASSERT_EQ(Idle.EnterConditions.Num(), 1);
    ASSERT_EQ(Idle.Transitions[0].Conditions.Num(), 1);
    EXPECT_TRUE(Idle.Transitions[0].Conditions[0].Get()->bInvert);
    ExpectEditRefused("statetree.add_node", R"("Type":"SStateTreeTestCondition","Name":"Late","State":"Idle","Transition":1)", "no transition 1");
}

TEST_F(MCPStateTreeToolsTest, EditsRefuseNamesTheTreeCannotTake)
{
    Edit("statetree.add_state", R"("State":{"Name":"Idle"})");
    Edit("statetree.add_node", R"("Type":"SStateTreeTestTask","Name":"Wait","State":"Idle")");

    ExpectEditRefused("statetree.add_state", R"("State":{"Name":"Idle"})", "already has a state named 'Idle'");
    ExpectEditRefused("statetree.add_state", R"("State":{"Name":"Child","Parent":"Ghost"})", "no state named 'Ghost'");
    ExpectEditRefused("statetree.add_node", R"("Type":"SStateTreeTestTask","Name":"Wait","State":"Idle")", "already has a node named 'Wait'");
    ExpectEditRefused("statetree.add_node", R"("Type":"SStateTreeTestTaskData","State":"Idle")", "No task, condition or evaluator");
    ExpectEditRefused("statetree.add_node", R"("Type":"SStateTreeTestTask","State":"Ghost")", "no state named 'Ghost'");
    ExpectEditRefused("statetree.add_node", R"("Type":"SStateTreeTestTask","State":"Idle","Properties":{"TicksToFinish":"Many"})", "TicksToFinish");
    ExpectEditRefused("statetree.set_binding", R"("Node":"Ghost","Binding":{"TargetField":"Input","SourceField":"Speed"})", "no node named 'Ghost'");
    ExpectEditRefused("statetree.add_transition", R"("State":"Ghost","Transition":{})", "no state named 'Ghost'");

    EXPECT_EQ(Tree->States.Num(), 1);
    EXPECT_EQ(Tree->States[0].Tasks.Num(), 1);
}

TEST_F(MCPStateTreeToolsTest, InstanceStateNeedsAWorldEditor)
{
    ExpectRefused("statetree.instance_state", R"({"Entity":"e1"})", "No world editor is open");
}
