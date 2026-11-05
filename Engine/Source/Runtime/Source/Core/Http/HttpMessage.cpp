#include "RuntimePCH.h"
#include "Core/Http/HttpMessage.h"

#include "Containers/StringFormat.h"

namespace Lumina::Http
{
    namespace
    {
        constexpr FStringView GHeaderTerminator = "\r\n\r\n";

        char ToLowerAscii(char Character)
        {
            return (Character >= 'A' && Character <= 'Z') ? static_cast<char>(Character - 'A' + 'a') : Character;
        }

        bool EqualsIgnoringCase(FStringView Left, FStringView Right)
        {
            if (Left.Len() != Right.Len())
            {
                return false;
            }

            for (int32 Index = 0; Index < Left.Len(); ++Index)
            {
                if (ToLowerAscii(Left[Index]) != ToLowerAscii(Right[Index]))
                {
                    return false;
                }
            }

            return true;
        }

        FStringView TrimSpace(FStringView Text)
        {
            int32 First = 0;
            while (First < Text.Len() && (Text[First] == ' ' || Text[First] == '\t'))
            {
                ++First;
            }

            int32 Last = Text.Len();
            while (Last > First && (Text[Last - 1] == ' ' || Text[Last - 1] == '\t'))
            {
                --Last;
            }

            return Text.Mid(First, Last - First);
        }

        bool ParseNonNegative(FStringView Text, int32& OutValue)
        {
            if (Text.IsEmpty() || Text.Len() > 10)
            {
                return false;
            }

            int64 Value = 0;
            for (char Character : Text)
            {
                if (Character < '0' || Character > '9')
                {
                    return false;
                }

                Value = Value * 10 + (Character - '0');
                if (Value > 0x7FFFFFFF)
                {
                    return false;
                }
            }

            OutValue = static_cast<int32>(Value);
            return true;
        }

        const char* ReasonForStatus(int32 Code)
        {
            switch (Code)
            {
            case 200: return "OK";
            case 202: return "Accepted";
            case 204: return "No Content";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 411: return "Length Required";
            case 413: return "Payload Too Large";
            case 415: return "Unsupported Media Type";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            default:  return "Unknown";
            }
        }
    }

    FStringView FRequest::FindHeader(FStringView Name) const
    {
        for (const FHeader& Header : Headers)
        {
            if (EqualsIgnoringCase(FStringView(Header.Name), Name))
            {
                return FStringView(Header.Value);
            }
        }

        return FStringView();
    }

    bool FRequest::WantsKeepAlive() const
    {
        const FStringView Connection = FindHeader("Connection");
        return !EqualsIgnoringCase(Connection, "close");
    }

    FResponse FResponse::Json(FString InBody)
    {
        FResponse Response;
        Response.StatusCode  = 200;
        Response.ContentType = "application/json";
        Response.Body        = Move(InBody);
        return Response;
    }

    FResponse FResponse::Text(int32 Code, FStringView Reason, FStringView InBody)
    {
        FResponse Response;
        Response.StatusCode   = Code;
        Response.ReasonPhrase = FString(Reason.GetData(), Reason.Len());
        Response.ContentType  = "text/plain; charset=utf-8";
        Response.Body         = FString(InBody.GetData(), InBody.Len());
        return Response;
    }

    FResponse FResponse::Empty(int32 Code, FStringView Reason)
    {
        FResponse Response;
        Response.StatusCode   = Code;
        Response.ReasonPhrase = FString(Reason.GetData(), Reason.Len());
        return Response;
    }

    FString FResponse::Serialize() const
    {
        const FString Reason = ReasonPhrase.IsEmpty() ? FString(ReasonForStatus(StatusCode)) : ReasonPhrase;

        FString Text = Lumina::Format("HTTP/1.1 {} {}\r\n", StatusCode, Reason);

        if (!ContentType.IsEmpty())
        {
            Text.Append(Lumina::Format("Content-Type: {}\r\n", ContentType));
        }

        // Always present, so a persistent connection knows where one message stops and the next starts.
        Text.Append(Lumina::Format("Content-Length: {}\r\n", Body.Len()));
        Text.Append(bKeepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");

        for (const FHeader& Header : ExtraHeaders)
        {
            Text.Append(Lumina::Format("{}: {}\r\n", Header.Name, Header.Value));
        }

        Text.Append("\r\n");
        Text.Append(Body);

        return Text;
    }

    EParseResult ParseRequest(FString& Buffer, FRequest& OutRequest, const FParseLimits& Limits)
    {
        const FStringView View(Buffer);

        const int32 HeaderEnd = View.Find(GHeaderTerminator);
        if (HeaderEnd == Constants::kIndexNone)
        {
            return View.Len() > Limits.MaxHeaderBytes
                ? EParseResult::Malformed
                : EParseResult::Incomplete;
        }

        if (static_cast<int32>(HeaderEnd) > Limits.MaxHeaderBytes)
        {
            return EParseResult::Malformed;
        }

        FStringView Head = View.Mid(0, HeaderEnd);

        // With no headers the terminator absorbs the request line's own break, leaving the line alone here.
        const int32 RequestLineEnd = Head.Find("\r\n");
        const FStringView RequestLine = RequestLineEnd == Constants::kIndexNone ? Head : Head.Mid(0, RequestLineEnd);

        const int32 FirstSpace = RequestLine.Find(' ');
        if (FirstSpace == Constants::kIndexNone)
        {
            return EParseResult::Malformed;
        }

        const int32 SecondSpace = RequestLine.Find(' ', FirstSpace + 1);
        if (SecondSpace == Constants::kIndexNone)
        {
            return EParseResult::Malformed;
        }

        const FStringView Method  = RequestLine.Mid(0, FirstSpace);
        const FStringView Target  = RequestLine.Mid(FirstSpace + 1, SecondSpace - FirstSpace - 1);
        const FStringView Version = RequestLine.Mid(SecondSpace + 1);

        if (Method.IsEmpty() || Target.IsEmpty() || Version.Mid(0, 5) != "HTTP/")
        {
            return EParseResult::Malformed;
        }

        FRequest Parsed;
        Parsed.Method = FString(Method.GetData(), Method.Len());
        Parsed.Target = FString(Target.GetData(), Target.Len());

        FStringView Rest = RequestLineEnd == Constants::kIndexNone
            ? FStringView()
            : Head.Mid(RequestLineEnd + 2);

        while (!Rest.IsEmpty())
        {
            const int32 LineEnd = Rest.Find("\r\n");
            const FStringView Line = Rest.Mid(0, LineEnd == Constants::kIndexNone ? Rest.Len() : LineEnd);

            const int32 Colon = Line.Find(':');
            if (Colon == Constants::kIndexNone)
            {
                return EParseResult::Malformed;
            }

            const FStringView Name  = TrimSpace(Line.Mid(0, Colon));
            const FStringView Value = TrimSpace(Line.Mid(Colon + 1));

            if (Name.IsEmpty())
            {
                return EParseResult::Malformed;
            }

            Parsed.Headers.Emplace(FString(Name.GetData(), Name.Len()), FString(Value.GetData(), Value.Len()));

            if (LineEnd == Constants::kIndexNone)
            {
                break;
            }

            Rest = Rest.Mid(LineEnd + 2);
        }

        // Chunked bodies are refused outright rather than half handled, since no client here needs them.
        const FStringView Encoding = Parsed.FindHeader("Transfer-Encoding");
        if (!Encoding.IsEmpty() && !EqualsIgnoringCase(Encoding, "identity"))
        {
            return EParseResult::Malformed;
        }

        int32 ContentLength = 0;
        const FStringView LengthText = Parsed.FindHeader("Content-Length");
        if (!LengthText.IsEmpty() && !ParseNonNegative(LengthText, ContentLength))
        {
            return EParseResult::Malformed;
        }

        if (ContentLength > Limits.MaxBodyBytes)
        {
            return EParseResult::Malformed;
        }

        const int32 BodyStart = HeaderEnd + GHeaderTerminator.Len();
        if (View.Len() - BodyStart < ContentLength)
        {
            return EParseResult::Incomplete;
        }

        Parsed.Body = FString(View.GetData() + BodyStart, ContentLength);

        // Whatever follows belongs to the next request on this connection and has to survive.
        Buffer = FString(View.GetData() + BodyStart + ContentLength,
                         View.Len() - BodyStart - ContentLength);

        OutRequest = Move(Parsed);
        return EParseResult::Complete;
    }
}
