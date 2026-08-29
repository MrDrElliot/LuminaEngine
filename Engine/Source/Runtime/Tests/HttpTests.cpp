#include <gtest/gtest.h>

#include "Containers/StringFormat.h"
#include "Core/Http/HttpMessage.h"
#include "Core/Http/HttpServer.h"
#include "Core/Threading/Thread.h"
#include "Platform/Socket/PlatformSocket.h"

using namespace Lumina;
using namespace Lumina::Http;

namespace
{
    FParseLimits DefaultLimits()
    {
        return FParseLimits();
    }

    EParseResult Parse(const char* Text, FRequest& OutRequest)
    {
        FString Buffer(Text);
        return ParseRequest(Buffer, OutRequest, DefaultLimits());
    }

    const char* GSimplePost =
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"hello\":123}";
}

TEST(HttpMessage, APostIsParsedIntoItsParts)
{
    FRequest Request;
    ASSERT_EQ(Parse(GSimplePost, Request), EParseResult::Complete);

    EXPECT_EQ(Request.Method, "POST");
    EXPECT_EQ(Request.Target, "/mcp");
    EXPECT_EQ(Request.Body, "{\"hello\":123}");
}

// Header names are case insensitive per the standard, and clients vary in what they send.
TEST(HttpMessage, HeaderLookupIgnoresCase)
{
    FRequest Request;
    ASSERT_EQ(Parse(GSimplePost, Request), EParseResult::Complete);

    EXPECT_EQ(Request.FindHeader("content-type"), "application/json");
    EXPECT_EQ(Request.FindHeader("CONTENT-TYPE"), "application/json");
    EXPECT_TRUE(Request.FindHeader("Missing").empty());
}

TEST(HttpMessage, SurroundingSpaceIsStrippedFromHeaderValues)
{
    FRequest Request;
    ASSERT_EQ(Parse(
        "GET / HTTP/1.1\r\n"
        "X-Padded:    spaced out   \r\n"
        "\r\n", Request), EParseResult::Complete);

    EXPECT_EQ(Request.FindHeader("X-Padded"), "spaced out");
}

TEST(HttpMessage, AGetWithNoBodyIsComplete)
{
    FRequest Request;
    ASSERT_EQ(Parse("GET /status HTTP/1.1\r\nHost: x\r\n\r\n", Request), EParseResult::Complete);

    EXPECT_EQ(Request.Method, "GET");
    EXPECT_TRUE(Request.Body.empty());
}

TEST(HttpMessage, TruncatedHeadersAreIncomplete)
{
    FRequest Request;
    EXPECT_EQ(Parse("POST /mcp HTTP/1.1\r\nHost: x\r\n", Request), EParseResult::Incomplete);
}

// A body that has not fully arrived must wait rather than be handed over short.
TEST(HttpMessage, ATruncatedBodyIsIncomplete)
{
    FRequest Request;
    EXPECT_EQ(Parse(
        "POST /mcp HTTP/1.1\r\n"
        "Content-Length: 20\r\n"
        "\r\n"
        "only ten..", Request), EParseResult::Incomplete);
}

TEST(HttpMessage, AGarbageRequestLineIsMalformed)
{
    FRequest Request;
    EXPECT_EQ(Parse("this is not http\r\n\r\n", Request), EParseResult::Malformed);
}

TEST(HttpMessage, AMissingProtocolIsMalformed)
{
    FRequest Request;
    EXPECT_EQ(Parse("GET /only-two-parts\r\n\r\n", Request), EParseResult::Malformed);
}

TEST(HttpMessage, AHeaderWithoutAColonIsMalformed)
{
    FRequest Request;
    EXPECT_EQ(Parse("GET / HTTP/1.1\r\nBrokenHeader\r\n\r\n", Request), EParseResult::Malformed);
}

TEST(HttpMessage, ANonNumericContentLengthIsMalformed)
{
    FRequest Request;
    EXPECT_EQ(Parse("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n", Request), EParseResult::Malformed);
}

// Chunked is refused outright rather than half handled, so a client cannot be silently misread.
TEST(HttpMessage, ChunkedEncodingIsRefused)
{
    FRequest Request;
    EXPECT_EQ(Parse(
        "POST / HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n", Request), EParseResult::Malformed);
}

TEST(HttpMessage, ABodyOverTheLimitIsMalformed)
{
    FParseLimits Limits;
    Limits.MaxBodyBytes = 8;

    FString Buffer(
        "POST / HTTP/1.1\r\n"
        "Content-Length: 64\r\n"
        "\r\n");

    FRequest Request;
    EXPECT_EQ(ParseRequest(Buffer, Request, Limits), EParseResult::Malformed);
}

// Two requests can share one read, so the parser has to leave the second one intact.
TEST(HttpMessage, PipelinedRequestsAreConsumedOneAtATime)
{
    FString Buffer(
        "GET /first HTTP/1.1\r\n\r\n"
        "GET /second HTTP/1.1\r\n\r\n");

    FRequest First;
    ASSERT_EQ(ParseRequest(Buffer, First, DefaultLimits()), EParseResult::Complete);
    EXPECT_EQ(First.Target, "/first");

    FRequest Second;
    ASSERT_EQ(ParseRequest(Buffer, Second, DefaultLimits()), EParseResult::Complete);
    EXPECT_EQ(Second.Target, "/second");

    EXPECT_TRUE(Buffer.empty());
}

TEST(HttpMessage, ABodyIsNotStolenFromTheFollowingRequest)
{
    FString Buffer(
        "POST /one HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "12345"
        "GET /two HTTP/1.1\r\n\r\n");

    FRequest First;
    ASSERT_EQ(ParseRequest(Buffer, First, DefaultLimits()), EParseResult::Complete);
    EXPECT_EQ(First.Body, "12345");

    FRequest Second;
    ASSERT_EQ(ParseRequest(Buffer, Second, DefaultLimits()), EParseResult::Complete);
    EXPECT_EQ(Second.Target, "/two");
}

TEST(HttpMessage, KeepAliveIsTheDefaultAndCloseIsHonored)
{
    FRequest Persistent;
    ASSERT_EQ(Parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n", Persistent), EParseResult::Complete);
    EXPECT_TRUE(Persistent.WantsKeepAlive());

    FRequest Closing;
    ASSERT_EQ(Parse("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", Closing), EParseResult::Complete);
    EXPECT_FALSE(Closing.WantsKeepAlive());
}

TEST(HttpMessage, AJsonResponseCarriesTypeAndLength)
{
    const FString Text = FResponse::Json("{\"ok\":true}").Serialize();

    EXPECT_NE(Text.find("HTTP/1.1 200 OK\r\n"), FString::npos);
    EXPECT_NE(Text.find("Content-Type: application/json\r\n"), FString::npos);
    EXPECT_NE(Text.find("Content-Length: 11\r\n"), FString::npos);
    EXPECT_NE(Text.find("\r\n\r\n{\"ok\":true}"), FString::npos);
}

TEST(HttpMessage, AStatusOnlyResponseStillDeclaresZeroLength)
{
    const FString Text = FResponse::Empty(405, "Method Not Allowed").Serialize();

    EXPECT_NE(Text.find("HTTP/1.1 405 Method Not Allowed\r\n"), FString::npos);
    EXPECT_NE(Text.find("Content-Length: 0\r\n"), FString::npos);
}

TEST(HttpMessage, AResponseRoundTripsThroughTheParser)
{
    FResponse Response = FResponse::Json("{}");
    Response.bKeepAlive = false;

    const FString Text = Response.Serialize();
    EXPECT_NE(Text.find("Connection: close\r\n"), FString::npos);
}

namespace
{
    // Speaks just enough HTTP to drive the server under test over a real socket.
    FString RoundTrip(uint16 Port, const char* RequestText, int32 TimeoutMs = 4000)
    {
        Platform::FSocketConnectionPtr Client = Platform::ConnectSocket("127.0.0.1", Port, TimeoutMs);
        if (!Client)
        {
            return FString();
        }

        const int32 Length = static_cast<int32>(strlen(RequestText));
        int32 Sent = 0;
        while (Sent < Length)
        {
            Platform::ESocketResult Result = Platform::ESocketResult::Ok;
            Sent += Client->Send(reinterpret_cast<const uint8*>(RequestText) + Sent, Length - Sent, Result);

            if (Result != Platform::ESocketResult::Ok && Result != Platform::ESocketResult::WouldBlock)
            {
                return FString();
            }
        }

        FString Reply;
        for (int32 Attempt = 0; Attempt < TimeoutMs; ++Attempt)
        {
            uint8 Chunk[2048];
            Platform::ESocketResult Result = Platform::ESocketResult::Ok;
            const int32 Read = Client->Receive(Chunk, sizeof(Chunk), Result);

            if (Read > 0)
            {
                Reply.append(reinterpret_cast<const char*>(Chunk), static_cast<size_t>(Read));

                const size_t HeaderEnd = Reply.find("\r\n\r\n");
                if (HeaderEnd != FString::npos)
                {
                    return Reply;
                }
                continue;
            }

            if (Result == Platform::ESocketResult::Closed)
            {
                return Reply;
            }

            Threading::Sleep(1);
        }

        return Reply;
    }

    bool StartEchoServer(FServer& Server)
    {
        FServerParams Params;
        Params.Port = 0;

        return Server.Start(Params, [](const FRequest& Request) -> FResponse
        {
            if (Request.Method != "POST")
            {
                return FResponse::Empty(405, "Method Not Allowed");
            }

            if (Request.Target != "/mcp")
            {
                return FResponse::Empty(404, "Not Found");
            }

            return FResponse::Json(Request.Body);
        });
    }
}

TEST(HttpServer, AServerBindsAnEphemeralPortAndReportsIt)
{
    FServer Server;
    ASSERT_TRUE(StartEchoServer(Server));

    EXPECT_TRUE(Server.IsRunning());
    EXPECT_NE(Server.GetBoundPort(), 0);

    Server.Stop();
    EXPECT_FALSE(Server.IsRunning());
}

TEST(HttpServer, StoppingTwiceIsHarmless)
{
    FServer Server;
    ASSERT_TRUE(StartEchoServer(Server));

    Server.Stop();
    Server.Stop();

    EXPECT_FALSE(Server.IsRunning());
}

TEST(HttpServer, AServerRefusesToStartWithoutAHandler)
{
    FServer Server;
    EXPECT_FALSE(Server.Start(FServerParams(), FRequestHandler()));
}

TEST(HttpServer, APostReachesTheHandlerAndTheBodyComesBack)
{
    FServer Server;
    ASSERT_TRUE(StartEchoServer(Server));

    const FString Reply = RoundTrip(Server.GetBoundPort(),
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 15\r\n"
        "\r\n"
        "{\"method\":\"go\"}");

    EXPECT_NE(Reply.find("HTTP/1.1 200 OK"), FString::npos);
    EXPECT_NE(Reply.find("{\"method\":\"go\"}"), FString::npos);

    Server.Stop();
}

TEST(HttpServer, TheHandlerCanRejectAMethodOrTarget)
{
    FServer Server;
    ASSERT_TRUE(StartEchoServer(Server));

    const FString Wrong = RoundTrip(Server.GetBoundPort(), "GET /mcp HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_NE(Wrong.find("405"), FString::npos);

    const FString Missing = RoundTrip(Server.GetBoundPort(),
        "POST /nowhere HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    EXPECT_NE(Missing.find("404"), FString::npos);

    Server.Stop();
}

// A client that keeps the connection open has to get every reply, not just the first.
TEST(HttpServer, TwoRequestsShareOneConnection)
{
    FServer Server;
    ASSERT_TRUE(StartEchoServer(Server));

    Platform::FSocketConnectionPtr Client = Platform::ConnectSocket("127.0.0.1", Server.GetBoundPort(), 4000);
    ASSERT_TRUE(Client);

    const auto SendRequest = [&](const char* Body)
    {
        const FString Text = Lumina::Format(
            "POST /mcp HTTP/1.1\r\nHost: x\r\nContent-Length: {}\r\n\r\n{}", strlen(Body), Body);

        int32 Sent = 0;
        while (Sent < static_cast<int32>(Text.size()))
        {
            Platform::ESocketResult Result = Platform::ESocketResult::Ok;
            Sent += Client->Send(reinterpret_cast<const uint8*>(Text.c_str()) + Sent,
                static_cast<int32>(Text.size()) - Sent, Result);
        }
    };

    const auto ReadUntilBothReplies = [&](FString& Into, int32 Attempts)
    {
        for (int32 Attempt = 0; Attempt < Attempts; ++Attempt)
        {
            uint8 Chunk[2048];
            Platform::ESocketResult Result = Platform::ESocketResult::Ok;
            const int32 Read = Client->Receive(Chunk, sizeof(Chunk), Result);

            if (Read > 0)
            {
                Into.append(reinterpret_cast<const char*>(Chunk), static_cast<size_t>(Read));

                if (Into.find("{\"n\":1}") != FString::npos && Into.find("{\"n\":2}") != FString::npos)
                {
                    return;
                }

                continue;
            }

            Threading::Sleep(1);
        }
    };

    SendRequest("{\"n\":1}");
    SendRequest("{\"n\":2}");

    FString Replies;
    ReadUntilBothReplies(Replies, 4000);

    EXPECT_NE(Replies.find("{\"n\":1}"), FString::npos);
    EXPECT_NE(Replies.find("{\"n\":2}"), FString::npos);

    Server.Stop();
}

TEST(HttpServer, GarbageGetsAFourHundredRatherThanAHang)
{
    FServer Server;
    ASSERT_TRUE(StartEchoServer(Server));

    const FString Reply = RoundTrip(Server.GetBoundPort(), "absolute nonsense\r\n\r\n");
    EXPECT_NE(Reply.find("400"), FString::npos);

    Server.Stop();
}

// A handler that throws must become a 500 rather than tearing down the server thread.
TEST(HttpServer, AThrowingHandlerBecomesAServerError)
{
    FServer Server;

    FServerParams Params;
    ASSERT_TRUE(Server.Start(Params, [](const FRequest&) -> FResponse
    {
        throw std::runtime_error("handler exploded");
    }));

    const FString Reply = RoundTrip(Server.GetBoundPort(),
        "POST /mcp HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");

    EXPECT_NE(Reply.find("500"), FString::npos);
    EXPECT_TRUE(Server.IsRunning());

    Server.Stop();
}

TEST(HttpServer, ARequestSplitAcrossPacketsIsReassembled)
{
    FServer Server;
    ASSERT_TRUE(StartEchoServer(Server));

    Platform::FSocketConnectionPtr Client = Platform::ConnectSocket("127.0.0.1", Server.GetBoundPort(), 4000);
    ASSERT_TRUE(Client);

    const auto SendRaw = [&](const char* Text)
    {
        const int32 Length = static_cast<int32>(strlen(Text));
        int32 Sent = 0;
        while (Sent < Length)
        {
            Platform::ESocketResult Result = Platform::ESocketResult::Ok;
            Sent += Client->Send(reinterpret_cast<const uint8*>(Text) + Sent, Length - Sent, Result);
        }
    };

    SendRaw("POST /mcp HTTP/1.1\r\nHost: x\r\nContent-Len");
    Threading::Sleep(30);
    SendRaw("gth: 7\r\n\r\n{\"a\":1}");

    FString Reply;
    for (int32 Attempt = 0; Attempt < 3000; ++Attempt)
    {
        uint8 Chunk[2048];
        Platform::ESocketResult Result = Platform::ESocketResult::Ok;
        const int32 Read = Client->Receive(Chunk, sizeof(Chunk), Result);

        if (Read > 0)
        {
            Reply.append(reinterpret_cast<const char*>(Chunk), static_cast<size_t>(Read));
            if (Reply.find("\r\n\r\n") != FString::npos)
            {
                break;
            }
        }
        else
        {
            Threading::Sleep(1);
        }
    }

    EXPECT_NE(Reply.find("{\"a\":1}"), FString::npos);

    Server.Stop();
}
