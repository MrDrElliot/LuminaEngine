#include <gtest/gtest.h>

#include <limits>

#include "Agent/AgentToolRegistry.h"
#include "MCPEditorSessionTools.h"
#include "Platform/Time/PlatformTime.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPEditorInputLimitsTestsInternal
{
    constexpr const char* GOwner = "MCPEditorInputLimitsTests";
    constexpr const char* GHoldLimitMessage = "HoldMilliseconds has to be at most";
    constexpr const char* GTimeoutLimitMessage = "TimeoutSeconds has to be between";

    class MCPEditorInputLimitsTest : public testing::Test
    {
    protected:

        void SetUp() override { RegisterEditorSessionTools(GOwner); }

        void TearDown() override
        {
            Agent::FToolRegistry::Get().UnregisterOwner(GOwner);
            UnregisterEditorSessionTools();
        }

        template<typename TParams, typename TResult>
        Agent::FToolResult CallFast(FStringView Name, const TParams& Params, TResult& Result)
        {
            Agent::FTool Tool;
            EXPECT_TRUE(Agent::FToolRegistry::Get().TryGetTool(Name, Tool));

            const double Start = PlatformTime::Seconds();
            const Agent::FToolResult ToolResult = Tool.Invoke(&Params, &Result);
            EXPECT_LT(PlatformTime::Seconds() - Start, 1.0);
            return ToolResult;
        }

        Agent::FToolResult SendInput(EEditorInputKind Kind, int32 HoldMilliseconds)
        {
            SEditorInputParams Params;
            Params.Kind = Kind;
            Params.HoldMilliseconds = HoldMilliseconds;
            SEditorInputResult Result;
            return CallFast("editor.input", Params, Result);
        }

        Agent::FToolResult ReloadScripts(float TimeoutSeconds)
        {
            SScriptReloadParams Params;
            Params.TimeoutSeconds = TimeoutSeconds;
            SScriptReloadResult Result;
            return CallFast("editor.reload_scripts", Params, Result);
        }
    };

    void ExpectLimitError(const Agent::FToolResult& Result, const char* Message)
    {
        EXPECT_TRUE(Result.bIsError) << *Result.Text;
        EXPECT_NE(Result.Text.Find(Message), Constants::kIndexNone) << *Result.Text;
    }
}

using namespace MCPEditorInputLimitsTestsInternal;

TEST_F(MCPEditorInputLimitsTest, AnExcessiveMouseHoldIsRejectedWithoutSleeping)
{
    ExpectLimitError(SendInput(EEditorInputKind::Mouse, 2000000000), GHoldLimitMessage);
}

TEST_F(MCPEditorInputLimitsTest, AnExcessiveKeyHoldIsRejectedWithoutSleeping)
{
    ExpectLimitError(SendInput(EEditorInputKind::Key, 2000000000), GHoldLimitMessage);
}

TEST_F(MCPEditorInputLimitsTest, DefaultInputHoldsBehaveAsBefore)
{
    EXPECT_EQ(SendInput(EEditorInputKind::Mouse, 0).Text.Find(GHoldLimitMessage), Constants::kIndexNone);
    EXPECT_EQ(SendInput(EEditorInputKind::Key, 0).Text.Find(GHoldLimitMessage), Constants::kIndexNone);
}

TEST_F(MCPEditorInputLimitsTest, AnOutOfRangeReloadTimeoutIsRejectedWithoutPolling)
{
    ExpectLimitError(ReloadScripts(std::numeric_limits<float>::quiet_NaN()), GTimeoutLimitMessage);
    ExpectLimitError(ReloadScripts(-1.0f), GTimeoutLimitMessage);
    ExpectLimitError(ReloadScripts(100000.0f), GTimeoutLimitMessage);
}

TEST_F(MCPEditorInputLimitsTest, DefaultReloadTimeoutBehavesAsBefore)
{
    SScriptReloadParams Params;
    EXPECT_EQ(ReloadScripts(Params.TimeoutSeconds).Text.Find(GTimeoutLimitMessage), Constants::kIndexNone);
}
