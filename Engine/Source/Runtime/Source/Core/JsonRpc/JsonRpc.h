#pragma once

#include "Containers/Function.h"
#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Core/Templates/Optional.h"
#include "Core/Threading/Sync.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

#include "nlohmann/json.hpp"

namespace Lumina::JsonRpc
{
    // Codes reserved by JSON-RPC 2.0. Anything from -32000 upward is free for application use.
    enum class EErrorCode : int32
    {
        ParseError     = -32700,
        InvalidRequest = -32600,
        MethodNotFound = -32601,
        InvalidParams  = -32602,
        InternalError  = -32603,
    };

    struct FError
    {
        int32          Code = static_cast<int32>(EErrorCode::InternalError);
        FString        Message;
        nlohmann::json Data;
    };

    struct FRequest
    {
        FString        Method;

        // An object or an array when the caller sent one, otherwise an empty object.
        nlohmann::json Params;

        // Echoed back untouched, so a client can match a reply to what it sent.
        nlohmann::json Id;

        // A request that arrived with no id at all wants no reply, and its result is dropped.
        bool bNotification = false;
    };

    struct FResponse
    {
        nlohmann::json    Result;
        TOptional<FError> Error;

        // Exported one by one, because a plugin registering a method has to reach these from its own DLL.
        NODISCARD static RUNTIME_API FResponse Success(nlohmann::json Value);
        NODISCARD static RUNTIME_API FResponse Failure(EErrorCode Code, FStringView Message);
        NODISCARD static RUNTIME_API FResponse Failure(EErrorCode Code, FStringView Message, nlohmann::json Data);
    };

    using FMethodHandler = TFunction<FResponse(const FRequest&)>;

    // Routes JSON-RPC 2.0 messages to registered methods, knowing nothing about the transport underneath.
    class RUNTIME_API FDispatcher
    {
    public:

        // Owner names the module or plugin, so unloading one can drop every method it added.
        void RegisterMethod(FStringView Owner, FStringView Method, FMethodHandler Handler);

        bool UnregisterMethod(FStringView Method);

        // Returns how many went away, which is what plugin unload and script reload both need.
        int32 UnregisterOwner(FStringView Owner);

        NODISCARD bool HasMethod(FStringView Method) const;
        NODISCARD int32 GetMethodCount() const;

        void ForEachMethod(const TFunction<void(FStringView Method, FStringView Owner)>& Functor) const;

        // The reply to send back, or an empty string when the message wanted no reply.
        NODISCARD FString HandleMessage(FStringView Message);

    private:

        struct FEntry
        {
            FString        Owner;
            FMethodHandler Handler;
        };

        // Unset when the message was a notification, which is answered with silence even after a failure.
        NODISCARD TOptional<nlohmann::json> HandleOne(const nlohmann::json& Message);

        // Copied out rather than called under the lock, so a handler may register or unregister freely.
        NODISCARD TOptional<FMethodHandler> FindHandler(const FString& Method) const;

        THashMap<FString, FEntry> Methods;

        mutable FSharedMutex MethodsMutex;
    };
}
