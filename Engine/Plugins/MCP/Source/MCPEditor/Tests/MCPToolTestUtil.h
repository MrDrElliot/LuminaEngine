#pragma once

#include <gtest/gtest.h>

#include "Agent/AgentToolRegistry.h"
#include "Containers/StringFormat.h"
#include "MCPServer.h"

namespace Lumina::MCP
{
    class FRegisteredToolsTest : public testing::Test
    {
    protected:

        FRegisteredToolsTest(const char* InOwner, void (*InRegister)(FStringView))
            : Owner(InOwner)
            , Register(InRegister)
        {
        }

        void SetUp() override { Register(Owner); }
        void TearDown() override { Agent::FToolRegistry::Get().UnregisterOwner(Owner); }

        FJsonValue CallRaw(FStringView Tool, FStringView Arguments)
        {
            const FServer::FReply Reply = Server.HandleMessage(FStringView(Format(
                R"({{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{{"name":"{}","arguments":{}}}}})", Tool, Arguments)), "");
            FJsonValue Parsed;
            EXPECT_TRUE(FJsonValue::Parse(FStringView(Reply.Body), Parsed)) << *Reply.Body;
            return Parsed;
        }

        FJsonValue Call(FStringView Tool, FStringView Arguments)
        {
            return FJsonValue(CallRaw(Tool, Arguments)["result"]);
        }

        FJsonValue ExpectSucceeded(FStringView Tool, FStringView Arguments)
        {
            const FJsonValue Result = Call(Tool, Arguments);
            EXPECT_FALSE(Result["isError"].AsBool(true)) << *FString(Result["content"][0]["text"].AsString());
            return Result;
        }

        void ExpectRefused(FStringView Tool, FStringView Arguments, const char* Expected)
        {
            const FJsonValue Result = Call(Tool, Arguments);
            const FString Text(Result["content"][0]["text"].AsString());
            EXPECT_TRUE(Result["isError"].AsBool(false)) << *Text;
            EXPECT_NE(Text.Find(Expected), Constants::kIndexNone) << *Text;
        }

        FServer Server;
        const char* Owner;
        void (*Register)(FStringView);
    };
}
