#include <gtest/gtest.h>

#include "Agent/AgentToolRegistry.h"
#include "Containers/StringFormat.h"
#include "MCPServer.h"
#include "World/Entity/Components/NameComponent.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPLogTestsInternal
{
    constexpr const char* GOwner = "MCPLogTests";

    struct FScopedOwner
    {
        ~FScopedOwner() { Agent::FToolRegistry::Get().UnregisterOwner(GOwner); }
    };

    TVector<TSharedPtr<const FTransaction>> Snapshot(const FLog& Log)
    {
        TVector<TSharedPtr<const FTransaction>> Transactions;
        TVector<FConnection> Connections;
        Log.Snapshot(Transactions, Connections);
        return Transactions;
    }
}

using namespace MCPLogTestsInternal;

TEST(MCPLog, ADispatchedRequestIsLoggedWithItsOutcomeAndSession)
{
    FServer Server;

    const FServer::FReply Opened = Server.HandleMessage(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"clientInfo":{"name":"tester","version":"1.0"}}})", "");
    ASSERT_FALSE(Opened.OpenedSession.IsEmpty());

    (void)Server.HandleMessage(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", FStringView(Opened.OpenedSession));
    (void)Server.HandleMessage(R"({"jsonrpc":"2.0","id":3,"method":"no/such/method"})", FStringView(Opened.OpenedSession));

    TVector<TSharedPtr<const FTransaction>> Transactions;
    TVector<FConnection> Connections;
    Server.GetLog().Snapshot(Transactions, Connections);

    ASSERT_EQ(Transactions.Num(), 3);
    EXPECT_EQ(Transactions[1]->Method, "ping");
    EXPECT_FALSE(Transactions[1]->IsError());
    EXPECT_EQ(Transactions[1]->Session, Opened.OpenedSession);
    EXPECT_FALSE(Transactions[1]->Response.IsEmpty());
    EXPECT_GE(Transactions[1]->DurationMs, 0.0);
    EXPECT_TRUE(Transactions[2]->IsError());

    ASSERT_EQ(Connections.Num(), 1);
    EXPECT_EQ(Connections[0].Client, "tester 1.0");
    EXPECT_EQ(Connections[0].Requests, 3u);
    EXPECT_EQ(Connections[0].Errors, 1u);

    Server.GetLog().RecordDisconnect(FStringView(Opened.OpenedSession));
    Server.GetLog().Snapshot(Transactions, Connections);
    EXPECT_FALSE(Connections[0].bOpen);
}

TEST(MCPLog, AFailingToolCallIsLoggedWithItsToolArgumentsAndError)
{
    FScopedOwner Cleanup;
    ASSERT_TRUE((Agent::FToolRegistry::Get().Register<SNameComponent, SNameComponent>(
        GOwner, "test.fails", "Always fails.", Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
        [](const SNameComponent&, SNameComponent&) { return Agent::FToolResult::Error("broken on purpose"); })));

    FServer Server;
    (void)Server.HandleMessage(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"test.fails","arguments":{"Name":"Torch"}}})", "");

    const TVector<TSharedPtr<const FTransaction>> Transactions = Snapshot(Server.GetLog());
    ASSERT_EQ(Transactions.Num(), 1);
    EXPECT_EQ(Transactions[0]->Method, "tools/call");
    EXPECT_EQ(Transactions[0]->Tool, "test.fails");
    EXPECT_NE(Transactions[0]->Arguments.Find("Torch"), Constants::kIndexNone);
    EXPECT_EQ(Transactions[0]->Error, "broken on purpose");
}

TEST(MCPLog, TheHistoryKeepsOnlyTheNewestEntriesAndClipsPayloads)
{
    FLog Log;

    const FString Huge(FLog::kMaxPayloadBytes * 2, 'x');
    for (int32 Index = 0; Index < FLog::kCapacity + 10; ++Index)
    {
        (void)Log.Record("", Format(R"({{"jsonrpc":"2.0","id":{},"method":"ping"}})", Index), FStringView(Huge), 0.0);
    }

    const TVector<TSharedPtr<const FTransaction>> Transactions = Snapshot(Log);
    ASSERT_EQ(Transactions.Num(), FLog::kCapacity);
    EXPECT_NE(Transactions[0]->Request.Find("\"id\":10,"), Constants::kIndexNone);
    EXPECT_LT(Transactions[0]->Response.Len(), FLog::kMaxPayloadBytes + 64);
}

TEST(MCPLog, ClippingBacksUpToTheStartOfASplitCharacter)
{
    EXPECT_EQ(ClipText("abcdef", 3), "abc [3 more bytes]");
    EXPECT_EQ(ClipText("a\xC3\xA9z", 2), "a [3 more bytes]");
    EXPECT_EQ(ClipText("a\xE2\x82\xACz", 2), "a [4 more bytes]");
    EXPECT_EQ(ClipText("a\xE2\x82\xACz", 3), "a [4 more bytes]");
    EXPECT_EQ(ClipText("a\xE2\x82\xACz", 4), "a\xE2\x82\xAC [1 more bytes]");
}
