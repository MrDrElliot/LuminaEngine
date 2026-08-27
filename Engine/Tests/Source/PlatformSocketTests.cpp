#include <gtest/gtest.h>

#include "Platform/Socket/PlatformSocket.h"

using namespace Lumina;
using namespace Lumina::Platform;

namespace
{
    constexpr int32 GConnectTimeoutMs = 2000;
    constexpr int32 GAcceptTimeoutMs  = 2000;

    FSocketListenerPtr MakeListener()
    {
        FSocketListenParams Params;
        Params.bLoopbackOnly = true;
        Params.Port          = 0;
        return CreateSocketListener(Params);
    }

    // Loopback delivery is prompt but not instantaneous, so a read may legitimately need another turn.
    int32 ReceiveWithRetry(ISocketConnection& Connection, uint8* Buffer, int32 Capacity, ESocketResult& OutResult)
    {
        for (int32 Attempt = 0; Attempt < 200; ++Attempt)
        {
            const int32 Read = Connection.Receive(Buffer, Capacity, OutResult);
            if (OutResult != ESocketResult::WouldBlock)
            {
                return Read;
            }
        }

        return 0;
    }

    bool SendAll(ISocketConnection& Connection, const char* Text)
    {
        const int32 Length = static_cast<int32>(strlen(Text));

        int32 Sent = 0;
        while (Sent < Length)
        {
            ESocketResult Result = ESocketResult::Ok;
            Sent += Connection.Send(reinterpret_cast<const uint8*>(Text) + Sent, Length - Sent, Result);

            if (Result != ESocketResult::Ok && Result != ESocketResult::WouldBlock)
            {
                return false;
            }
        }

        return true;
    }
}

TEST(PlatformSocket, SocketsAreSupported)
{
    EXPECT_TRUE(IsSocketSupported());
}

// Port zero asks the operating system to pick, and the caller has no other way to learn which.
TEST(PlatformSocket, AnEphemeralPortIsReportedBack)
{
    const FSocketListenerPtr Listener = MakeListener();

    ASSERT_TRUE(Listener);
    EXPECT_NE(Listener->GetBoundPort(), 0);
}

TEST(PlatformSocket, AcceptGivesUpWhenNobodyConnects)
{
    const FSocketListenerPtr Listener = MakeListener();
    ASSERT_TRUE(Listener);

    const FSocketConnectionPtr Connection = Listener->Accept(50);
    EXPECT_FALSE(Connection);
}

TEST(PlatformSocket, AConnectionIsAcceptedAndDescribesItsPeer)
{
    const FSocketListenerPtr Listener = MakeListener();
    ASSERT_TRUE(Listener);

    const FSocketConnectionPtr Client = ConnectSocket("127.0.0.1", Listener->GetBoundPort(), GConnectTimeoutMs);
    ASSERT_TRUE(Client);
    EXPECT_TRUE(Client->IsOpen());

    const FSocketConnectionPtr Served = Listener->Accept(GAcceptTimeoutMs);
    ASSERT_TRUE(Served);
    EXPECT_TRUE(Served->IsOpen());
    EXPECT_FALSE(Served->GetPeerDescription().empty());
}

TEST(PlatformSocket, BytesTravelFromClientToServer)
{
    const FSocketListenerPtr Listener = MakeListener();
    ASSERT_TRUE(Listener);

    const FSocketConnectionPtr Client = ConnectSocket("127.0.0.1", Listener->GetBoundPort(), GConnectTimeoutMs);
    ASSERT_TRUE(Client);

    const FSocketConnectionPtr Served = Listener->Accept(GAcceptTimeoutMs);
    ASSERT_TRUE(Served);

    ASSERT_TRUE(SendAll(*Client, "ping"));

    uint8 Buffer[64] = {};
    ESocketResult Result = ESocketResult::Failed;
    const int32 Read = ReceiveWithRetry(*Served, Buffer, sizeof(Buffer), Result);

    EXPECT_EQ(Result, ESocketResult::Ok);
    ASSERT_EQ(Read, 4);
    EXPECT_EQ(memcmp(Buffer, "ping", 4), 0);
}

TEST(PlatformSocket, BytesTravelFromServerToClient)
{
    const FSocketListenerPtr Listener = MakeListener();
    ASSERT_TRUE(Listener);

    const FSocketConnectionPtr Client = ConnectSocket("127.0.0.1", Listener->GetBoundPort(), GConnectTimeoutMs);
    ASSERT_TRUE(Client);

    const FSocketConnectionPtr Served = Listener->Accept(GAcceptTimeoutMs);
    ASSERT_TRUE(Served);

    ASSERT_TRUE(SendAll(*Served, "pong"));

    uint8 Buffer[64] = {};
    ESocketResult Result = ESocketResult::Failed;
    const int32 Read = ReceiveWithRetry(*Client, Buffer, sizeof(Buffer), Result);

    EXPECT_EQ(Result, ESocketResult::Ok);
    ASSERT_EQ(Read, 4);
    EXPECT_EQ(memcmp(Buffer, "pong", 4), 0);
}

// An idle but healthy connection has to be distinguishable from one that ended.
TEST(PlatformSocket, AnIdleConnectionReportsWouldBlockRatherThanClosed)
{
    const FSocketListenerPtr Listener = MakeListener();
    ASSERT_TRUE(Listener);

    const FSocketConnectionPtr Client = ConnectSocket("127.0.0.1", Listener->GetBoundPort(), GConnectTimeoutMs);
    ASSERT_TRUE(Client);

    const FSocketConnectionPtr Served = Listener->Accept(GAcceptTimeoutMs);
    ASSERT_TRUE(Served);

    uint8 Buffer[64] = {};
    ESocketResult Result = ESocketResult::Failed;
    const int32 Read = Served->Receive(Buffer, sizeof(Buffer), Result);

    EXPECT_EQ(Result, ESocketResult::WouldBlock);
    EXPECT_EQ(Read, 0);
}

TEST(PlatformSocket, AHangUpIsReportedAsClosed)
{
    const FSocketListenerPtr Listener = MakeListener();
    ASSERT_TRUE(Listener);

    FSocketConnectionPtr Client = ConnectSocket("127.0.0.1", Listener->GetBoundPort(), GConnectTimeoutMs);
    ASSERT_TRUE(Client);

    const FSocketConnectionPtr Served = Listener->Accept(GAcceptTimeoutMs);
    ASSERT_TRUE(Served);

    Client->Close();
    EXPECT_FALSE(Client->IsOpen());

    uint8 Buffer[64] = {};
    ESocketResult Result = ESocketResult::Ok;
    ReceiveWithRetry(*Served, Buffer, sizeof(Buffer), Result);

    EXPECT_EQ(Result, ESocketResult::Closed);
}

// Data already in flight when the peer hangs up still has to arrive before the close is reported.
TEST(PlatformSocket, DataSentBeforeAHangUpIsStillDelivered)
{
    const FSocketListenerPtr Listener = MakeListener();
    ASSERT_TRUE(Listener);

    FSocketConnectionPtr Client = ConnectSocket("127.0.0.1", Listener->GetBoundPort(), GConnectTimeoutMs);
    ASSERT_TRUE(Client);

    const FSocketConnectionPtr Served = Listener->Accept(GAcceptTimeoutMs);
    ASSERT_TRUE(Served);

    ASSERT_TRUE(SendAll(*Client, "last words"));
    Client->Close();

    uint8 Buffer[64] = {};
    ESocketResult Result = ESocketResult::Failed;
    const int32 Read = ReceiveWithRetry(*Served, Buffer, sizeof(Buffer), Result);

    EXPECT_EQ(Result, ESocketResult::Ok);
    EXPECT_EQ(Read, 10);
}

TEST(PlatformSocket, ClosingIsIdempotent)
{
    const FSocketListenerPtr Listener = MakeListener();
    ASSERT_TRUE(Listener);

    const FSocketConnectionPtr Client = ConnectSocket("127.0.0.1", Listener->GetBoundPort(), GConnectTimeoutMs);
    ASSERT_TRUE(Client);

    Client->Close();
    Client->Close();

    EXPECT_FALSE(Client->IsOpen());
}

TEST(PlatformSocket, ConnectingWhereNothingListensFails)
{
    uint16 DeadPort = 0;
    {
        const FSocketListenerPtr Listener = MakeListener();
        ASSERT_TRUE(Listener);
        DeadPort = Listener->GetBoundPort();
    }

    const FSocketConnectionPtr Client = ConnectSocket("127.0.0.1", DeadPort, 250);
    EXPECT_FALSE(Client);
}

TEST(PlatformSocket, AMalformedHostIsRejected)
{
    const FSocketConnectionPtr Client = ConnectSocket("not-an-address", 1234, 250);
    EXPECT_FALSE(Client);
}

// Binding the same explicit port twice has to fail rather than silently sharing it.
TEST(PlatformSocket, ASecondListenerCannotTakeTheSamePort)
{
    const FSocketListenerPtr First = MakeListener();
    ASSERT_TRUE(First);

    FSocketListenParams Params;
    Params.bLoopbackOnly = true;
    Params.Port          = First->GetBoundPort();

    const FSocketListenerPtr Second = CreateSocketListener(Params);
    EXPECT_FALSE(Second);
}

TEST(PlatformSocket, ClosingTheListenerLeavesAnAcceptedConnectionUsable)
{
    FSocketListenerPtr Listener = MakeListener();
    ASSERT_TRUE(Listener);

    const FSocketConnectionPtr Client = ConnectSocket("127.0.0.1", Listener->GetBoundPort(), GConnectTimeoutMs);
    ASSERT_TRUE(Client);

    const FSocketConnectionPtr Served = Listener->Accept(GAcceptTimeoutMs);
    ASSERT_TRUE(Served);

    Listener->Close();

    ASSERT_TRUE(SendAll(*Client, "still here"));

    uint8 Buffer[64] = {};
    ESocketResult Result = ESocketResult::Failed;
    const int32 Read = ReceiveWithRetry(*Served, Buffer, sizeof(Buffer), Result);

    EXPECT_EQ(Result, ESocketResult::Ok);
    EXPECT_EQ(Read, 10);
}

TEST(PlatformSocket, ListenersOnSeparateEphemeralPortsCoexist)
{
    const FSocketListenerPtr First  = MakeListener();
    const FSocketListenerPtr Second = MakeListener();

    ASSERT_TRUE(First);
    ASSERT_TRUE(Second);
    EXPECT_NE(First->GetBoundPort(), Second->GetBoundPort());
}
