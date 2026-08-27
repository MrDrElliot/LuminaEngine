#include "RuntimePCH.h"
#include "Core/JsonRpc/JsonRpc.h"

#include "Core/Threading/Thread.h"
#include "Log/Log.h"

#include <exception>
#include <string>

namespace Lumina::JsonRpc
{
    namespace
    {
        constexpr const char* GProtocolVersion = "2.0";

        std::string ToStandard(FStringView Text)
        {
            return std::string(Text.data(), Text.size());
        }

        FString ToEngine(const std::string& Text)
        {
            return FString(Text.c_str(), Text.size());
        }

        FString Dump(const nlohmann::json& Value)
        {
            return ToEngine(Value.dump());
        }

        nlohmann::json MakeErrorObject(const FError& Error)
        {
            nlohmann::json Object = nlohmann::json::object();
            Object["code"]    = Error.Code;
            Object["message"] = ToStandard(Error.Message);

            if (!Error.Data.is_null())
            {
                Object["data"] = Error.Data;
            }

            return Object;
        }

        nlohmann::json MakeEnvelope(const nlohmann::json& Id, const FResponse& Response)
        {
            nlohmann::json Envelope = nlohmann::json::object();
            Envelope["jsonrpc"] = GProtocolVersion;
            Envelope["id"]      = Id;

            if (Response.Error.IsSet())
            {
                Envelope["error"] = MakeErrorObject(*Response.Error);
            }
            else
            {
                Envelope["result"] = Response.Result;
            }

            return Envelope;
        }

        // An id may be a string, a number or null, and anything else makes the whole request invalid.
        bool IsUsableId(const nlohmann::json& Id)
        {
            return Id.is_string() || Id.is_number() || Id.is_null();
        }
    }

    FResponse FResponse::Success(nlohmann::json Value)
    {
        FResponse Response;
        Response.Result = Move(Value);
        return Response;
    }

    FResponse FResponse::Failure(EErrorCode Code, FStringView Message)
    {
        return Failure(Code, Message, nlohmann::json());
    }

    FResponse FResponse::Failure(EErrorCode Code, FStringView Message, nlohmann::json Data)
    {
        FError Error;
        Error.Code    = static_cast<int32>(Code);
        Error.Message = FString(Message.data(), Message.size());
        Error.Data    = Move(Data);

        FResponse Response;
        Response.Error = Move(Error);
        return Response;
    }

    void FDispatcher::RegisterMethod(FStringView Owner, FStringView Method, FMethodHandler Handler)
    {
        if (Method.empty() || !Handler)
        {
            LOG_WARN("[JsonRpc] Refused a registration with an empty name or a null handler.");
            return;
        }

        FEntry Entry;
        Entry.Owner   = FString(Owner.data(), Owner.size());
        Entry.Handler = Move(Handler);

        const FString Key(Method.data(), Method.size());

        FWriteScopeLock Lock(MethodsMutex);

        if (Methods.find(Key) != Methods.end())
        {
            LOG_WARN("[JsonRpc] '{}' is already registered and the newer handler replaced it.", Key);
        }

        Methods[Key] = Move(Entry);
    }

    bool FDispatcher::UnregisterMethod(FStringView Method)
    {
        const FString Key(Method.data(), Method.size());

        FWriteScopeLock Lock(MethodsMutex);

        const auto Found = Methods.find(Key);
        if (Found == Methods.end())
        {
            return false;
        }

        Methods.erase(Found);
        return true;
    }

    int32 FDispatcher::UnregisterOwner(FStringView Owner)
    {
        const FString Match(Owner.data(), Owner.size());

        FWriteScopeLock Lock(MethodsMutex);

        int32 Removed = 0;
        for (auto It = Methods.begin(); It != Methods.end(); )
        {
            if (It->second.Owner == Match)
            {
                It = Methods.erase(It);
                ++Removed;
            }
            else
            {
                ++It;
            }
        }

        return Removed;
    }

    bool FDispatcher::HasMethod(FStringView Method) const
    {
        const FString Key(Method.data(), Method.size());

        FReadScopeLock Lock(MethodsMutex);
        return Methods.find(Key) != Methods.end();
    }

    int32 FDispatcher::GetMethodCount() const
    {
        FReadScopeLock Lock(MethodsMutex);
        return static_cast<int32>(Methods.size());
    }

    void FDispatcher::ForEachMethod(const TFunction<void(FStringView Method, FStringView Owner)>& Functor) const
    {
        if (!Functor)
        {
            return;
        }

        FReadScopeLock Lock(MethodsMutex);
        for (const auto& Pair : Methods)
        {
            Functor(FStringView(Pair.first), FStringView(Pair.second.Owner));
        }
    }

    TOptional<FMethodHandler> FDispatcher::FindHandler(const FString& Method) const
    {
        FReadScopeLock Lock(MethodsMutex);

        const auto Found = Methods.find(Method);
        if (Found == Methods.end())
        {
            return NullOpt;
        }

        return Found->second.Handler;
    }

    TOptional<nlohmann::json> FDispatcher::HandleOne(const nlohmann::json& Message)
    {
        if (!Message.is_object())
        {
            return MakeEnvelope(nlohmann::json(),
                FResponse::Failure(EErrorCode::InvalidRequest, "A request has to be a JSON object."));
        }

        // No id member at all is what makes a message a notification, which is different from a null id.
        const bool bNotification = !Message.contains("id");
        const nlohmann::json Id  = bNotification ? nlohmann::json() : Message["id"];

        const auto Reply = [&](const FResponse& Response) -> TOptional<nlohmann::json>
        {
            if (bNotification)
            {
                return NullOpt;
            }

            return MakeEnvelope(Id, Response);
        };

        if (!IsUsableId(Id))
        {
            return MakeEnvelope(nlohmann::json(),
                FResponse::Failure(EErrorCode::InvalidRequest, "An id has to be a string, a number or null."));
        }

        const auto Version = Message.find("jsonrpc");
        if (Version == Message.end() || !Version->is_string() || Version->get_ref<const std::string&>() != GProtocolVersion)
        {
            return Reply(FResponse::Failure(EErrorCode::InvalidRequest, "Only JSON-RPC 2.0 is accepted."));
        }

        const auto Method = Message.find("method");
        if (Method == Message.end() || !Method->is_string())
        {
            return Reply(FResponse::Failure(EErrorCode::InvalidRequest, "A request needs a string method."));
        }

        FRequest Request;
        Request.Method        = ToEngine(Method->get_ref<const std::string&>());
        Request.Id            = Id;
        Request.bNotification = bNotification;
        Request.Params        = nlohmann::json::object();

        const auto Params = Message.find("params");
        if (Params != Message.end())
        {
            if (!Params->is_object() && !Params->is_array())
            {
                return Reply(FResponse::Failure(EErrorCode::InvalidParams, "Params has to be an object or an array."));
            }

            Request.Params = *Params;
        }

        TOptional<FMethodHandler> Handler = FindHandler(Request.Method);
        if (!Handler.IsSet())
        {
            return Reply(FResponse::Failure(EErrorCode::MethodNotFound, "No handler is registered for that method."));
        }

        FResponse Response;

        // A throwing handler must not take the editor down with it, so it becomes an internal error instead.
        try
        {
            Response = (*Handler)(Request);
        }
        catch (const std::exception& Exception)
        {
            LOG_ERROR("[JsonRpc] '{}' threw. {}", Request.Method, Exception.what());
            Response = FResponse::Failure(EErrorCode::InternalError, "The handler threw an exception.");
        }
        catch (...)
        {
            LOG_ERROR("[JsonRpc] '{}' threw an unrecognized exception.", Request.Method);
            Response = FResponse::Failure(EErrorCode::InternalError, "The handler threw an unrecognized exception.");
        }

        return Reply(Response);
    }

    FString FDispatcher::HandleMessage(FStringView Message)
    {
        const nlohmann::json Parsed = nlohmann::json::parse(
            Message.data(), Message.data() + Message.size(), nullptr, false);

        if (Parsed.is_discarded())
        {
            return Dump(MakeEnvelope(nlohmann::json(),
                FResponse::Failure(EErrorCode::ParseError, "The message was not valid JSON.")));
        }

        if (!Parsed.is_array())
        {
            const TOptional<nlohmann::json> Envelope = HandleOne(Parsed);
            return Envelope.IsSet() ? Dump(*Envelope) : FString();
        }

        if (Parsed.empty())
        {
            return Dump(MakeEnvelope(nlohmann::json(),
                FResponse::Failure(EErrorCode::InvalidRequest, "A batch has to hold at least one request.")));
        }

        nlohmann::json Replies = nlohmann::json::array();
        for (const nlohmann::json& Element : Parsed)
        {
            const TOptional<nlohmann::json> Envelope = HandleOne(Element);
            if (Envelope.IsSet())
            {
                Replies.push_back(*Envelope);
            }
        }

        // A batch holding nothing but notifications is answered with silence rather than an empty array.
        return Replies.empty() ? FString() : Dump(Replies);
    }
}
