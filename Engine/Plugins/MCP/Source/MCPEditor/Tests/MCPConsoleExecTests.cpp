#include "MCPEditorSessionTools.h"
#include "MCPToolTestUtil.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPConsoleExecTestsInternal
{
    class MCPConsoleExecTest : public FRegisteredToolsTest
    {
    protected:

        MCPConsoleExecTest() : FRegisteredToolsTest("MCPConsoleExecTests", &RegisterEditorSessionTools) {}

        void TearDown() override
        {
            FRegisteredToolsTest::TearDown();
            UnregisterEditorSessionTools();
        }

        // The line is spliced into the request as written, so tabs arrive as JSON escapes.
        static FString ExecArguments(FStringView JsonLine)
        {
            return Format(R"({{"Line":"{}"}})", JsonLine);
        }
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
        ExpectRefused("editor.console_exec", FStringView(ExecArguments(Line)), Expected);
    }
}

TEST_F(MCPConsoleExecTest, APaddedVariableReadReportsTheTrimmedName)
{
    const FJsonValue Result = Call("editor.console_exec", FStringView(ExecArguments(R"(\tmcp.Port )")));
    EXPECT_FALSE(Result["isError"].AsBool(true));
    EXPECT_EQ(Result["content"][0]["text"].AsString().Find("mcp.Port = "), 0);
}
