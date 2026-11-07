#include <gtest/gtest.h>

#include "Agent/AgentToolRegistry.h"
#include "Containers/StringFormat.h"
#include "MCPLookDevTools.h"
#include "MCPServer.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPLookDevToolsTestsInternal
{
    constexpr const char* GOwner = "MCPLookDevToolsTests";

    class MCPLookDevToolsTest : public testing::Test
    {
    protected:

        void SetUp() override { RegisterLookDevTools(GOwner); }
        void TearDown() override { Agent::FToolRegistry::Get().UnregisterOwner(GOwner); }

        FJsonValue Call(FStringView Tool, FStringView Arguments)
        {
            const FServer::FReply Reply = Server.HandleMessage(FStringView(Format(
                R"({{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{{"name":"{}","arguments":{}}}}})", Tool, Arguments)), "");
            FJsonValue Parsed;
            EXPECT_TRUE(FJsonValue::Parse(FStringView(Reply.Body), Parsed)) << *Reply.Body;
            return FJsonValue(Parsed["result"]);
        }

        void ExpectRefused(FStringView Tool, FStringView Arguments, const char* Expected)
        {
            const FJsonValue Result = Call(Tool, Arguments);
            const FString Text(Result["content"][0]["text"].AsString());
            EXPECT_TRUE(Result["isError"].AsBool(false)) << *Text;
            EXPECT_NE(Text.Find(Expected), Constants::kIndexNone) << *Text;
        }

        FServer Server;
    };
}

using namespace MCPLookDevToolsTestsInternal;

TEST_F(MCPLookDevToolsTest, CaptureRefusesAnUnknownPreset)
{
    ExpectRefused("lookdev.capture", R"({"Label":"T0","Presets":["ContactClose","Sideways"]})", "No preset is named 'Sideways'");
}

TEST_F(MCPLookDevToolsTest, CaptureRefusesZeroFrames)
{
    ExpectRefused("lookdev.capture", R"({"Label":"T0","FrameCount":0})", "FrameCount is 0");
}

TEST_F(MCPLookDevToolsTest, CaptureRefusesALabelThatIsNotOneFolder)
{
    ExpectRefused("lookdev.capture", R"({"Label":"a/b"})", "cannot name a folder");
    ExpectRefused("lookdev.capture", R"({"Label":""})", "cannot name a folder");
}

TEST_F(MCPLookDevToolsTest, BuildRefusesAnElevationOutsideTheSky)
{
    ExpectRefused("lookdev.build_scene", R"({"SunElevationDegrees":0})", "SunElevationDegrees is 0");
    ExpectRefused("lookdev.build_scene", R"({"SunElevationDegrees":-5})", "SunElevationDegrees is -5");
    ExpectRefused("lookdev.build_scene", R"({"SunElevationDegrees":91})", "SunElevationDegrees is 91");
}
