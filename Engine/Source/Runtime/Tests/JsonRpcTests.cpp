#include <gtest/gtest.h>

#include "Core/JsonRpc/JsonRpc.h"

using namespace Lumina;
using namespace Lumina::JsonRpc;

namespace
{
    FJsonValue Handle(FDispatcher& Dispatcher, const char* Message)
    {
        const FString Reply = Dispatcher.HandleMessage(FStringView(Message));
        if (Reply.IsEmpty())
        {
            return FJsonValue();
        }

        FJsonValue Parsed;
        EXPECT_TRUE(FJsonValue::Parse(Reply.View(), Parsed)) << *Reply;
        return Parsed;
    }

    // FDispatcher owns a mutex, so it is neither copyable nor movable and has to be filled in place.
    void InstallEcho(FDispatcher& Dispatcher)
    {
        Dispatcher.RegisterMethod("Tests", "echo", [](const FRequest& Request)
        {
            return FResponse::Success(Request.Params);
        });
    }

    int32 ErrorCodeOf(const FJsonValue& Reply)
    {
        return Reply["error"]["code"].AsInt32();
    }
}

TEST(JsonRpc, ACallReachesItsHandlerAndEchoesTheId)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher,
        R"({"jsonrpc":"2.0","method":"echo","params":{"value":7},"id":42})");

    EXPECT_EQ(Reply["jsonrpc"], "2.0");
    EXPECT_EQ(Reply["id"], 42);
    EXPECT_EQ(Reply["result"]["value"], 7);
    EXPECT_FALSE(Reply.Contains("error"));
}

TEST(JsonRpc, AStringIdSurvivesUnchanged)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher,
        R"({"jsonrpc":"2.0","method":"echo","id":"abc"})");

    EXPECT_EQ(Reply["id"], "abc");
}

// A missing id member is what marks a notification, and the spec forbids answering one.
TEST(JsonRpc, ANotificationIsAnsweredWithSilence)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FString Reply = Dispatcher.HandleMessage(FStringView(R"({"jsonrpc":"2.0","method":"echo"})"));
    EXPECT_TRUE(Reply.IsEmpty());
}

// A null id is a real id, so unlike an absent one it still gets a reply.
TEST(JsonRpc, ANullIdIsNotANotification)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher,
        R"({"jsonrpc":"2.0","method":"echo","id":null})");

    EXPECT_TRUE(Reply.Contains("result"));
    EXPECT_TRUE(Reply["id"].IsNull());
}

TEST(JsonRpc, AFailingNotificationIsStillSilent)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FString Reply = Dispatcher.HandleMessage(FStringView(R"({"jsonrpc":"2.0","method":"missing"})"));
    EXPECT_TRUE(Reply.IsEmpty());
}

TEST(JsonRpc, MalformedJsonReportsAParseErrorAgainstANullId)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher, R"({"jsonrpc":"2.0",)");

    EXPECT_EQ(ErrorCodeOf(Reply), static_cast<int32>(EErrorCode::ParseError));
    EXPECT_TRUE(Reply["id"].IsNull());
}

TEST(JsonRpc, AnUnknownMethodReportsMethodNotFound)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher, R"({"jsonrpc":"2.0","method":"nope","id":1})");
    EXPECT_EQ(ErrorCodeOf(Reply), static_cast<int32>(EErrorCode::MethodNotFound));
}

TEST(JsonRpc, TheWrongProtocolVersionIsRejected)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher, R"({"jsonrpc":"1.0","method":"echo","id":1})");
    EXPECT_EQ(ErrorCodeOf(Reply), static_cast<int32>(EErrorCode::InvalidRequest));
}

TEST(JsonRpc, AMissingMethodIsRejected)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher, R"({"jsonrpc":"2.0","id":1})");
    EXPECT_EQ(ErrorCodeOf(Reply), static_cast<int32>(EErrorCode::InvalidRequest));
}

TEST(JsonRpc, ScalarParamsAreRejected)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher,
        R"({"jsonrpc":"2.0","method":"echo","params":5,"id":1})");

    EXPECT_EQ(ErrorCodeOf(Reply), static_cast<int32>(EErrorCode::InvalidParams));
}

TEST(JsonRpc, ArrayParamsAreAccepted)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher,
        R"({"jsonrpc":"2.0","method":"echo","params":[1,2],"id":1})");

    EXPECT_EQ(Reply["result"][0], 1);
    EXPECT_EQ(Reply["result"][1], 2);
}

TEST(JsonRpc, OmittedParamsArriveAsAnEmptyObject)
{
    FDispatcher Dispatcher;
    Dispatcher.RegisterMethod("Tests", "shape", [](const FRequest& Request)
    {
        FJsonValue Result = FJsonValue::MakeObject();
        Result.Set("object", Request.Params.IsObject());
        Result.Set("empty", Request.Params.IsEmpty());
        return FResponse::Success(Result);
    });

    const FJsonValue Reply = Handle(Dispatcher, R"({"jsonrpc":"2.0","method":"shape","id":1})");

    EXPECT_TRUE(Reply["result"]["object"].AsBool());
    EXPECT_TRUE(Reply["result"]["empty"].AsBool());
}

TEST(JsonRpc, ANonObjectRequestIsRejected)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher, R"("just a string")");
    EXPECT_EQ(ErrorCodeOf(Reply), static_cast<int32>(EErrorCode::InvalidRequest));
}

TEST(JsonRpc, AnObjectIdIsRejected)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher,
        R"({"jsonrpc":"2.0","method":"echo","id":{"bad":true}})");

    EXPECT_EQ(ErrorCodeOf(Reply), static_cast<int32>(EErrorCode::InvalidRequest));
}

// A handler that throws has to become an error object rather than unwinding into the transport.
TEST(JsonRpc, AThrowingHandlerBecomesAnInternalError)
{
    FDispatcher Dispatcher;
    Dispatcher.RegisterMethod("Tests", "boom", [](const FRequest&) -> FResponse
    {
        throw std::runtime_error("handler exploded");
    });

    const FJsonValue Reply = Handle(Dispatcher, R"({"jsonrpc":"2.0","method":"boom","id":1})");
    EXPECT_EQ(ErrorCodeOf(Reply), static_cast<int32>(EErrorCode::InternalError));
}

TEST(JsonRpc, AHandlerCanReportItsOwnFailure)
{
    FDispatcher Dispatcher;
    Dispatcher.RegisterMethod("Tests", "refuse", [](const FRequest&)
    {
        return FResponse::Failure(EErrorCode::InvalidParams, "That entity does not exist.");
    });

    const FJsonValue Reply = Handle(Dispatcher, R"({"jsonrpc":"2.0","method":"refuse","id":1})");

    EXPECT_EQ(ErrorCodeOf(Reply), static_cast<int32>(EErrorCode::InvalidParams));
    EXPECT_EQ(Reply["error"]["message"], "That entity does not exist.");
    EXPECT_FALSE(Reply["error"].Contains("data"));
}

TEST(JsonRpc, ErrorDataIsCarriedThroughWhenSupplied)
{
    FDispatcher Dispatcher;
    Dispatcher.RegisterMethod("Tests", "detail", [](const FRequest&)
    {
        FJsonValue Data = FJsonValue::MakeObject();
        Data.Set("expected", "int32");
        return FResponse::Failure(EErrorCode::InvalidParams, "Wrong type.", Data);
    });

    const FJsonValue Reply = Handle(Dispatcher, R"({"jsonrpc":"2.0","method":"detail","id":1})");
    EXPECT_EQ(Reply["error"]["data"]["expected"], "int32");
}

TEST(JsonRpc, ABatchAnswersEveryCallInOrder)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher,
        R"([{"jsonrpc":"2.0","method":"echo","params":{"n":1},"id":1},)"
        R"({"jsonrpc":"2.0","method":"echo","params":{"n":2},"id":2}])");

    ASSERT_TRUE(Reply.IsArray());
    ASSERT_EQ(Reply.Num(), 2);
    EXPECT_EQ(Reply[0]["id"], 1);
    EXPECT_EQ(Reply[1]["id"], 2);
}

// Notifications drop out of a batch, so the reply array is shorter than the request array.
TEST(JsonRpc, ABatchOmitsNotificationsFromItsReply)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher,
        R"([{"jsonrpc":"2.0","method":"echo"},)"
        R"({"jsonrpc":"2.0","method":"echo","id":9}])");

    ASSERT_TRUE(Reply.IsArray());
    ASSERT_EQ(Reply.Num(), 1);
    EXPECT_EQ(Reply[0]["id"], 9);
}

TEST(JsonRpc, ABatchOfOnlyNotificationsIsAnsweredWithSilence)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FString Reply = Dispatcher.HandleMessage(FStringView(
        R"([{"jsonrpc":"2.0","method":"echo"},{"jsonrpc":"2.0","method":"echo"}])"));

    EXPECT_TRUE(Reply.IsEmpty());
}

TEST(JsonRpc, AnEmptyBatchIsRejected)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher, "[]");
    EXPECT_EQ(ErrorCodeOf(Reply), static_cast<int32>(EErrorCode::InvalidRequest));
}

// A malformed member of a batch fails on its own without taking the rest down.
TEST(JsonRpc, ABatchIsolatesABadMember)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    const FJsonValue Reply = Handle(Dispatcher,
        R"([{"jsonrpc":"2.0","method":"echo","id":1},5])");

    ASSERT_EQ(Reply.Num(), 2);
    EXPECT_TRUE(Reply[0].Contains("result"));
    EXPECT_EQ(ErrorCodeOf(Reply[1]), static_cast<int32>(EErrorCode::InvalidRequest));
}

TEST(JsonRpc, RegistrationIsVisibleAndCountable)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    EXPECT_TRUE(Dispatcher.HasMethod("echo"));
    EXPECT_FALSE(Dispatcher.HasMethod("nope"));
    EXPECT_EQ(Dispatcher.GetMethodCount(), 1);
}

TEST(JsonRpc, UnregisteringOneMethodLeavesTheRest)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);
    Dispatcher.RegisterMethod("Tests", "second", [](const FRequest&)
    {
        return FResponse::Success(FJsonValue());
    });

    EXPECT_TRUE(Dispatcher.UnregisterMethod("echo"));
    EXPECT_FALSE(Dispatcher.UnregisterMethod("echo"));
    EXPECT_FALSE(Dispatcher.HasMethod("echo"));
    EXPECT_TRUE(Dispatcher.HasMethod("second"));
}

// Plugin unload and script reload lean on this, so an owner has to take exactly its own methods away.
TEST(JsonRpc, UnregisteringAnOwnerDropsOnlyItsMethods)
{
    FDispatcher Dispatcher;

    const auto Empty = [](const FRequest&) { return FResponse::Success(FJsonValue()); };

    Dispatcher.RegisterMethod("PluginA", "a.one", Empty);
    Dispatcher.RegisterMethod("PluginA", "a.two", Empty);
    Dispatcher.RegisterMethod("PluginB", "b.one", Empty);

    EXPECT_EQ(Dispatcher.UnregisterOwner("PluginA"), 2);
    EXPECT_EQ(Dispatcher.GetMethodCount(), 1);
    EXPECT_TRUE(Dispatcher.HasMethod("b.one"));
    EXPECT_EQ(Dispatcher.UnregisterOwner("PluginA"), 0);
}

TEST(JsonRpc, ForEachMethodReportsTheOwner)
{
    FDispatcher Dispatcher;
    InstallEcho(Dispatcher);

    FString SeenMethod;
    FString SeenOwner;
    int32 Count = 0;

    Dispatcher.ForEachMethod([&](FStringView Method, FStringView Owner)
    {
        SeenMethod = FString(Method.GetData(), Method.Len());
        SeenOwner  = FString(Owner.GetData(), Owner.Len());
        ++Count;
    });

    EXPECT_EQ(Count, 1);
    EXPECT_EQ(SeenMethod, "echo");
    EXPECT_EQ(SeenOwner, "Tests");
}

TEST(JsonRpc, ANullHandlerIsRefused)
{
    FDispatcher Dispatcher;
    Dispatcher.RegisterMethod("Tests", "bad", FMethodHandler());

    EXPECT_EQ(Dispatcher.GetMethodCount(), 0);
}

// A handler may register another one, which deadlocks if dispatch holds the lock while calling out.
TEST(JsonRpc, AHandlerMayRegisterDuringDispatch)
{
    FDispatcher Dispatcher;
    Dispatcher.RegisterMethod("Tests", "grow", [&Dispatcher](const FRequest&)
    {
        Dispatcher.RegisterMethod("Tests", "grown", [](const FRequest&)
        {
            return FResponse::Success(FJsonValue());
        });

        return FResponse::Success(FJsonValue());
    });

    Handle(Dispatcher, R"({"jsonrpc":"2.0","method":"grow","id":1})");

    EXPECT_TRUE(Dispatcher.HasMethod("grown"));
}
