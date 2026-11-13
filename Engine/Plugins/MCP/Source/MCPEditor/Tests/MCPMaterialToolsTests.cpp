#include "MCPMaterialTools.h"
#include "MCPToolTestUtil.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Material/MaterialOps.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPMaterialToolsTestsInternal
{
    class MCPMaterialToolsTest : public FRegisteredToolsTest
    {
    protected:

        MCPMaterialToolsTest() : FRegisteredToolsTest("MCPMaterialToolsTests", &RegisterMaterialTools) {}

        void SetUp() override
        {
            RegisterPendingTypes();
            FRegisteredToolsTest::SetUp();
            const FString Package = Format("/MCPMaterialToolsTests/{}", testing::UnitTest::GetInstance()->current_test_info()->name());
            Material = NewObject<CMaterial>(CPackage::CreatePackage(Package), "Scratch");
            Guid = FString(*Material->GetGUID().ToString());
        }

        CMaterialNodeGraph* Graph() const { return MaterialOps::FindOrCreateGraph(Material); }

        int64 AddNode(const char* Type)
        {
            const FJsonValue Result = ExpectSucceeded("material.node",
                FStringView(Format(R"({{"Material":"{}","NodeType":"{}"}})", Guid, Type)));
            return Result["structuredContent"]["Id"].AsInt64();
        }

        CEdNodeGraphPin* OutputPin(const char* Name) const
        {
            return NodeGraphOps::FindPin(MaterialOps::FindOutputNode(Graph()), Name, ENodePinDirection::Input);
        }

        CMaterial* Material = nullptr;
        FString Guid;
    };
}

using namespace MCPMaterialToolsTestsInternal;

TEST_F(MCPMaterialToolsTest, CreateRefusesAFolderOutsideScannedContent)
{
    ExpectRefused("material.create", R"({"Folder":"/Game/Scripts","Name":"M_Scratch"})", "/Game/Content");
}

TEST_F(MCPMaterialToolsTest, NodeAddsAndRemoves)
{
    const int64 Constant = AddNode("CMaterialExpression_ConstantFloat");
    ASSERT_NE(NodeGraphOps::FindNode(Graph(), Constant), nullptr);

    ExpectSucceeded("material.node", FStringView(Format(R"({{"Material":"{}","Action":"Remove","Node":{}}})", Guid, Constant)));
    EXPECT_EQ(NodeGraphOps::FindNode(Graph(), Constant), nullptr);

    ExpectRefused("material.node", FStringView(Format(R"({{"Material":"{}","NodeType":"CNoSuchNode"}})", Guid)),
        "No material node type is named 'CNoSuchNode'");
}

TEST_F(MCPMaterialToolsTest, LinkConnectsAndDisconnects)
{
    const int64 Constant = AddNode("CMaterialExpression_ConstantFloat");
    const int64 Output   = MaterialOps::FindOutputNode(Graph())->GetNodeID();

    ExpectSucceeded("material.link", FStringView(Format(
        R"({{"Material":"{}","FromNode":{},"FromPin":"X","ToNode":{},"ToPin":"Metallic"}})", Guid, Constant, Output)));
    ASSERT_TRUE(OutputPin("Metallic")->HasConnection());

    ExpectSucceeded("material.link", FStringView(Format(
        R"({{"Material":"{}","Action":"Disconnect","Node":{},"Pin":"Metallic"}})", Guid, Output)));
    EXPECT_FALSE(OutputPin("Metallic")->HasConnection());
}

TEST_F(MCPMaterialToolsTest, SetNodePropertyWritesTheField)
{
    const int64 Constant = AddNode("CMaterialExpression_ConstantFloat");

    const FJsonValue Result = ExpectSucceeded("material.set_node_property", FStringView(Format(
        R"({{"Material":"{}","Node":{},"Path":"ParameterName","Value":"Roughness"}})", Guid, Constant)));
    EXPECT_NE(FString(Result["structuredContent"]["Current"].AsString()).Find("Roughness"), Constants::kIndexNone);

    const FJsonValue Described = ExpectSucceeded("material.describe", FStringView(Format(R"({{"Material":"{}"}})", Guid)));
    FString Values;
    for (const FJsonValue& Node : Described["structuredContent"]["Nodes"].AsArray())
    {
        if (Node["Id"].AsInt64() == Constant)
        {
            Values = FString(Node["Values"].AsString());
        }
    }
    EXPECT_NE(Values.Find("Roughness"), Constants::kIndexNone) << *Values;

    ExpectRefused("material.set_node_property", FStringView(Format(
        R"({{"Material":"{}","Node":987654,"Path":"ParameterName","Value":"Roughness"}})", Guid)), "No node 987654");
}

TEST_F(MCPMaterialToolsTest, CompileOfAnEmptyGraphReportsTheMissingShaderCompiler)
{
    ExpectRefused("material.compile", FStringView(Format(R"({{"Material":"{}"}})", Guid)),
        "No Shader Compiler No shader compiler is running in this process.");
}
