#include "RuntimePCH.h"
#include "Core/JsonRpc/JsonRpc.h"

#include "Log/Log.h"

#include <exception>

namespace Lumina::JsonRpc
{
    namespace
    {
        constexpr const char* GProtocolVersion = "2.0";

        FJsonValue MakeErrorObject(const FError& Error)
        {
            FJsonValue Object = FJsonValue::MakeObject(3);
            Object.Set("code", Error.Code);
            Object.Set("message", Error.Message);
            if (!Error.Data.IsNull())
            {
                Object.Set("data", Error.Data);
            }
            return Object;
        }

        FJsonValue MakeEnvelope(const FJsonValue& Id, const FResponse& Response)
        {
            FJsonValue Envelope = FJsonValue::MakeObject(3);
            Envelope.Set("jsonrpc", GProtocolVersion);
            Envelope.Set("id", Id);
            if (Response.Error.IsSet())
            {
                Envelope.Set("error", MakeErrorObject(*Response.Error));
            }
            else
            {
                Envelope.Set("result", Response.Result);
            }
            return Envelope;
        }

        // An id may be a string, a number or null, and anything else makes the whole request invalid.
        bool IsUsableId(const FJsonValue& Id)
        {
            return Id.IsString() || Id.IsNumber() || Id.IsNull();
        }
    }

    FResponse FResponse::Success(FJsonValue Value)
    {
        FResponse Response;
        Response.Result = Move(Value);
        return Response;
    }

    FResponse FResponse::Failure(EErrorCode Code, FStringView Message)
    {
        return Failure(Code, Message, FJsonValue());
    }

    FResponse FResponse::Failure(EErrorCode Code, FStringView Message, FJsonValue Data)
    {
        FError Error;
        Error.Code    = static_cast<int32>(Code);
        Error.Message = FString(Message.GetData(), Message.Len());
        Error.Data    = Move(Data);

        FResponse Response;
        Response.Error = Move(Error);
        return Response;
    }

    void FDispatcher::RegisterMethod(FStringView Owner, FStringView Method, FMethodHandler Handler)
    {
        if (Method.IsEmpty() || !Handler)
        {
            LOG_WARN("[JsonRpc] Refused a registration with an empty name or a null handler.");
            return;
        }

        FEntry Entry;
        Entry.Owner   = FString(Owner.GetData(), Owner.Len());
        Entry.Handler = Move(Handler);

        const FString Key(Method.GetData(), Method.Len());

        auto Guard = Methods.Write();

        if (Guard->Contains(Key))
        {
            LOG_WARN("[JsonRpc] '{}' is already registered and the newer handler replaced it.", Key);
        }

        Guard->Add(Key, Move(Entry));
    }

    bool FDispatcher::UnregisterMethod(FStringView Method)
    {
        const FString Key(Method.GetData(), Method.Len());

        auto Guard = Methods.Write();

        const auto Found = Guard->Find(Key);
        if (Found == nullptr)
        {
            return false;
        }

        Guard->Remove(Key);
        return true;
    }

    int32 FDispatcher::UnregisterOwner(FStringView Owner)
    {
        const FString Match(Owner.GetData(), Owner.Len());

        auto Guard = Methods.Write();

        int32 Removed = 0;
        for (auto It = Guard->begin(); It != Guard->end(); )
        {
            if (It->Value.Owner == Match)
            {
                It = Guard->Remove(It);
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
        const FString Key(Method.GetData(), Method.Len());

        auto Guard = Methods.Read();
        return Guard->Contains(Key);
    }

    int32 FDispatcher::GetMethodCount() const
    {
        auto Guard = Methods.Read();
        return Guard->Num();
    }

    void FDispatcher::ForEachMethod(const TFunction<void(FStringView Method, FStringView Owner)>& Functor) const
    {
        if (!Functor)
        {
            return;
        }

        auto Guard = Methods.Read();
        for (const auto& [Method, Entry] : *Guard)
        {
            Functor(FStringView(Method), FStringView(Entry.Owner));
        }
    }

    TOptional<FMethodHandler> FDispatcher::FindHandler(const FString& Method) const
    {
        auto Guard = Methods.Read();

        const auto Found = Guard->Find(Method);
        if (Found == nullptr)
        {
            return NullOpt;
        }

        return Found->Handler;
    }

    TOptional<FJsonValue> FDispatcher::HandleOne(const FJsonValue& Message)
    {
        if (!Message.IsObject())
        {
            return MakeEnvelope(FJsonValue(),
                FResponse::Failure(EErrorCode::InvalidRequest, "A request has to be a JSON object."));
        }

        // No id member at all is what makes a message a notification, which is different from a null id.
        const FJsonValue* IdMember = Message.Find("id");
        const bool bNotification   = IdMember == nullptr;
        const FJsonValue Id        = bNotification ? FJsonValue() : *IdMember;

        const auto Reply = [&](const FResponse& Response) -> TOptional<FJsonValue>
        {
            if (bNotification)
            {
                return NullOpt;
            }

            return MakeEnvelope(Id, Response);
        };

        if (!IsUsableId(Id))
        {
            return MakeEnvelope(FJsonValue(),
                FResponse::Failure(EErrorCode::InvalidRequest, "An id has to be a string, a number or null."));
        }

        const FJsonValue& Version = Message["jsonrpc"];
        if (!Version.IsString() || Version.AsString() != GProtocolVersion)
        {
            return Reply(FResponse::Failure(EErrorCode::InvalidRequest, "Only JSON-RPC 2.0 is accepted."));
        }

        const FJsonValue& Method = Message["method"];
        if (!Method.IsString())
        {
            return Reply(FResponse::Failure(EErrorCode::InvalidRequest, "A request needs a string method."));
        }

        FRequest Request;
        Request.Method        = FString(Method.AsString());
        Request.Id            = Id;
        Request.bNotification = bNotification;
        Request.Params        = FJsonValue::MakeObject();

        if (const FJsonValue* Params = Message.Find("params"))
        {
            if (!Params->IsObject() && !Params->IsArray())
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
        FJsonValue Parsed;
        if (!FJsonValue::Parse(Message, Parsed))
        {
            return MakeEnvelope(FJsonValue(),
                FResponse::Failure(EErrorCode::ParseError, "The message was not valid JSON.")).ToString();
        }

        if (!Parsed.IsArray())
        {
            const TOptional<FJsonValue> Envelope = HandleOne(Parsed);
            return Envelope.IsSet() ? Envelope->ToString() : FString();
        }

        if (Parsed.IsEmpty())
        {
            return MakeEnvelope(FJsonValue(),
                FResponse::Failure(EErrorCode::InvalidRequest, "A batch has to hold at least one request.")).ToString();
        }

        FJsonValue Replies = FJsonValue::MakeArray(Parsed.Num());
        for (const FJsonValue& Element : Parsed.AsArray())
        {
            TOptional<FJsonValue> Envelope = HandleOne(Element);
            if (Envelope.IsSet())
            {
                Replies.Add(Move(*Envelope));
            }
        }

        // A batch holding nothing but notifications is answered with silence rather than an empty array.
        return Replies.IsEmpty() ? FString() : Replies.ToString();
    }
}
