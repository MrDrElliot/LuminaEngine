#include <gtest/gtest.h>

#include <stdexcept>

#include "Agent/AgentToolRegistry.h"
#include "Containers/StringFormat.h"
#include "MCPDataTableTools.h"
#include "MCPToolTestUtil.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPServerTestsInternal
{
    constexpr const char* GOwner = "MCPServerTests";

    class MCPServerTest : public FRegisteredToolsTest
    {
    protected:

        MCPServerTest() : FRegisteredToolsTest(GOwner, [](FStringView) {}) {}

        // Echoes the RawJson Value field back as the structured result, so a test sees what the tool was handed.
        static void RegisterEcho()
        {
            ASSERT_TRUE((Agent::FToolRegistry::Get().Register<SSetRowPropertyParams, SRowPropertyResult>(
                GOwner, "test.echo", "Echoes Value.", Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
                [](const SSetRowPropertyParams& Params, SRowPropertyResult& Result)
                {
                    Result.Current = Params.Value;
                    return Agent::FToolResult::Ok("echoed");
                })));
        }

        FJsonValue EchoedValue(FStringView Value)
        {
            const FJsonValue Result = Call("test.echo", FStringView(Format(R"({{"Value":{}}})", Value)));
            EXPECT_FALSE(Result["isError"].AsBool(true));
            return FJsonValue(Result["structuredContent"]["Current"]);
        }
    };
}

using namespace MCPServerTestsInternal;

TEST_F(MCPServerTest, ToolsListGivesEachToolItsNameDescriptionAndSchemas)
{
    RegisterEcho();
    ASSERT_TRUE((Agent::FToolRegistry::Get().Register<SSetRowPropertyParams>(
        GOwner, "test.textonly", "Answers in text.", Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
        [](const SSetRowPropertyParams&) { return Agent::FToolResult::Ok("text"); })));

    const FServer::FReply Reply = Server.HandleMessage(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", "");
    FJsonValue Response;
    ASSERT_TRUE(FJsonValue::Parse(FStringView(Reply.Body), Response)) << *Reply.Body;

    const FJsonValue* Echo = nullptr;
    const FJsonValue* TextOnly = nullptr;
    for (const FJsonValue& Entry : Response["result"]["tools"].AsArray())
    {
        if (Entry["name"].AsString() == "test.echo")
        {
            Echo = &Entry;
        }
        else if (Entry["name"].AsString() == "test.textonly")
        {
            TextOnly = &Entry;
        }
    }

    ASSERT_NE(Echo, nullptr);
    EXPECT_EQ((*Echo)["description"].AsString(), "Echoes Value.");
    EXPECT_TRUE((*Echo)["inputSchema"].IsObject());
    EXPECT_TRUE((*Echo)["outputSchema"].IsObject());

    ASSERT_NE(TextOnly, nullptr);
    EXPECT_TRUE((*TextOnly)["inputSchema"].IsObject());
    EXPECT_FALSE(TextOnly->Contains("outputSchema"));
}

TEST_F(MCPServerTest, AnUnknownToolIsMethodNotFound)
{
    EXPECT_EQ(CallRaw("test.nosuchtool", "{}")["error"]["code"].AsInt32(), static_cast<int32>(JsonRpc::EErrorCode::MethodNotFound));
}

TEST_F(MCPServerTest, ArgumentsThatDoNotFitTheParamsAreInvalidParams)
{
    RegisterEcho();

    EXPECT_EQ(CallRaw("test.echo", R"({"Bogus":1})")["error"]["code"].AsInt32(), static_cast<int32>(JsonRpc::EErrorCode::InvalidParams));
    EXPECT_EQ(CallRaw("test.echo", R"({"Asset":5})")["error"]["code"].AsInt32(), static_cast<int32>(JsonRpc::EErrorCode::InvalidParams));
}

TEST_F(MCPServerTest, AThrowingToolReportsAnErrorWithoutStructuredContent)
{
    ASSERT_TRUE((Agent::FToolRegistry::Get().Register<SSetRowPropertyParams, SRowPropertyResult>(
        GOwner, "test.throws", "Always throws.", Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
        [](const SSetRowPropertyParams&, SRowPropertyResult&) -> Agent::FToolResult { throw std::runtime_error("kaboom"); })));

    const FJsonValue Result = Call("test.throws", "{}");
    EXPECT_TRUE(Result["isError"].AsBool());
    EXPECT_NE(Result["content"][0]["text"].AsString().Find("The tool failed: kaboom"), Constants::kIndexNone);
    EXPECT_FALSE(Result.Contains("structuredContent"));
}

TEST_F(MCPServerTest, ASuccessfulToolWithAResultTypeReturnsStructuredContent)
{
    RegisterEcho();

    const FJsonValue Result = Call("test.echo", R"({"Value":"7"})");
    EXPECT_FALSE(Result["isError"].AsBool(true));
    EXPECT_EQ(Result["content"][0]["text"].AsString(), "echoed");
    EXPECT_EQ(Result["structuredContent"]["Current"].AsString(), "7");
}

TEST_F(MCPServerTest, RawJsonArgumentsAreEncodedAsJsonText)
{
    RegisterEcho();

    EXPECT_EQ(EchoedValue(R"({"Speed":2})").AsString(), R"({"Speed":2})");
    EXPECT_EQ(EchoedValue(R"("[1,2]")").AsString(), "[1,2]");
    EXPECT_EQ(EchoedValue(R"("hello")").AsString(), R"("hello")");
}
