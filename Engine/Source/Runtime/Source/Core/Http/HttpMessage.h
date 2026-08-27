#pragma once

#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina::Http
{
    struct FHeader
    {
        FString Name;
        FString Value;
    };

    struct FRequest
    {
        FString Method;
        FString Target;
        FString Body;

        TVector<FHeader> Headers;

        // Header names are matched without regard to case, as the standard requires. Empty when absent.
        NODISCARD RUNTIME_API FStringView FindHeader(FStringView Name) const;

        // HTTP 1.1 keeps a connection open unless the peer asked otherwise.
        NODISCARD RUNTIME_API bool WantsKeepAlive() const;
    };

    struct FResponse
    {
        int32   StatusCode = 200;
        FString ReasonPhrase;
        FString ContentType;
        FString Body;

        TVector<FHeader> ExtraHeaders;

        bool bKeepAlive = true;

        NODISCARD RUNTIME_API static FResponse Json(FString InBody);
        NODISCARD RUNTIME_API static FResponse Text(int32 Code, FStringView Reason, FStringView InBody);
        NODISCARD RUNTIME_API static FResponse Empty(int32 Code, FStringView Reason);

        NODISCARD RUNTIME_API FString Serialize() const;
    };

    enum class EParseResult : uint8
    {
        // The buffer holds only part of a message, so the caller should read more and try again.
        Incomplete,

        Complete,

        // The bytes cannot be a valid request, so the connection has to be dropped rather than resynced.
        Malformed,
    };

    struct FParseLimits
    {
        int32 MaxHeaderBytes = 64 * 1024;
        int32 MaxBodyBytes   = 8 * 1024 * 1024;
    };

    // Removes one complete request from the front of Buffer, leaving anything after it for the next call.
    NODISCARD RUNTIME_API EParseResult ParseRequest(FString& Buffer, FRequest& OutRequest, const FParseLimits& Limits);
}
