#include <gtest/gtest.h>

#include "Agent/AgentToolRegistry.h"
#include "Containers/StringFormat.h"
#include "MCPEditorSessionTools.h"
#include "MCPServer.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPConsoleExecTestsInternal
{
    constexpr const char* GOwner = "MCPConsoleExecTests";

    class MCPConsoleExecTest : public testing::Test
    {
    protected:

        void SetUp() override { RegisterEditorSessionTools(GOwner); }

        void TearDown() override
        {
            Agent::FToolRegistry::Get().UnregisterOwner(GOwner);
            UnregisterEditorSessionTools();
        }

        // The line is spliced into the request as written, so tabs arrive as JSON escapes.
        FJsonValue Exec(FStringView JsonLine)
        {
            const FServer::FReply Reply = Server.HandleMessage(FStringView(Format(
                R"({{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{{"name":"editor.console_exec","arguments":{{"Line":"{}"}}}}}})",
                JsonLine)), "");
            FJsonValue Parsed;
            EXPECT_TRUE(FJsonValue::Parse(FStringView(Reply.Body), Parsed)) << *Reply.Body;
            return FJsonValue(Parsed["result"]);
        }

        FServer Server;
    };
}

using namespace MCPConsoleExecTestsInternal;

TEST_F(MCPConsoleExecTest, ADeniedCommandPaddedWithSpacesOrTabsIsStillRefused)
{
    const TPair<const char*, const char*> Cases[] = {
        { R"(quit\t)", "'quit' is not allowed" },
        { R"(\tquit)", "'quit' is not allowed" },
        { " quit ", "'quit' is not allowed" },
        { R"(exit\t)", "'exit' is not allowed" },
        { R"(\t crash)", "'crash' is not allowed" },
    };
    for (const auto& [Line, Expected] : Cases)
    {
        const FJsonValue Result = Exec(Line);
        EXPECT_TRUE(Result["isError"].AsBool(false)) << Line;
        EXPECT_NE(Result["content"][0]["text"].AsString().Find(Expected), Constants::kIndexNone) << Line;
    }
}

TEST_F(MCPConsoleExecTest, APaddedVariableReadReportsTheTrimmedName)
{
    const FJsonValue Result = Exec(R"(\tmcp.Port )");
    EXPECT_FALSE(Result["isError"].AsBool(true));
    EXPECT_EQ(Result["content"][0]["text"].AsString().Find("mcp.Port = "), 0);
}
