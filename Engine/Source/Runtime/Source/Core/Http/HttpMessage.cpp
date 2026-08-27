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
            if (Left.size() != Right.size())
            {
                return false;
            }

            for (size_t Index = 0; Index < Left.size(); ++Index)
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
            size_t First = 0;
            while (First < Text.size() && (Text[First] == ' ' || Text[First] == '\t'))
            {
                ++First;
            }

            size_t Last = Text.size();
            while (Last > First && (Text[Last - 1] == ' ' || Text[Last - 1] == '\t'))
            {
                --Last;
            }

            return Text.substr(First, Last - First);
        }

        bool ParseNonNegative(FStringView Text, int32& OutValue)
        {
            if (Text.empty() || Text.size() > 10)
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
        Response.ReasonPhrase = FString(Reason.data(), Reason.size());
        Response.ContentType  = "text/plain; charset=utf-8";
        Response.Body         = FString(InBody.data(), InBody.size());
        return Response;
    }

    FResponse FResponse::Empty(int32 Code, FStringView Reason)
    {
        FResponse Response;
        Response.StatusCode   = Code;
        Response.ReasonPhrase = FString(Reason.data(), Reason.size());
        return Response;
    }

    FString FResponse::Serialize() const
    {
        const FString Reason = ReasonPhrase.empty() ? FString(ReasonForStatus(StatusCode)) : ReasonPhrase;

        FString Text = Lumina::Format("HTTP/1.1 {} {}\r\n", StatusCode, Reason);

        if (!ContentType.empty())
        {
            Text.append(Lumina::Format("Content-Type: {}\r\n", ContentType));
        }

        // Always present, so a persistent connection knows where one message stops and the next starts.
        Text.append(Lumina::Format("Content-Length: {}\r\n", Body.size()));
        Text.append(bKeepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");

        for (const FHeader& Header : ExtraHeaders)
        {
            Text.append(Lumina::Format("{}: {}\r\n", Header.Name, Header.Value));
        }

        Text.append("\r\n");
        Text.append(Body);

        return Text;
    }

    EParseResult ParseRequest(FString& Buffer, FRequest& OutRequest, const FParseLimits& Limits)
    {
        const FStringView View(Buffer);

        const size_t HeaderEnd = View.find(GHeaderTerminator);
        if (HeaderEnd == FStringView::npos)
        {
            return static_cast<int32>(View.size()) > Limits.MaxHeaderBytes
                ? EParseResult::Malformed
                : EParseResult::Incomplete;
        }

        if (static_cast<int32>(HeaderEnd) > Limits.MaxHeaderBytes)
        {
            return EParseResult::Malformed;
        }

        FStringView Head = View.substr(0, HeaderEnd);

        // With no headers the terminator absorbs the request line's own break, leaving the line alone here.
        const size_t RequestLineEnd = Head.find("\r\n");
        const FStringView RequestLine = RequestLineEnd == FStringView::npos ? Head : Head.substr(0, RequestLineEnd);

        const size_t FirstSpace = RequestLine.find(' ');
        if (FirstSpace == FStringView::npos)
        {
            return EParseResult::Malformed;
        }

        const size_t SecondSpace = RequestLine.find(' ', FirstSpace + 1);
        if (SecondSpace == FStringView::npos)
        {
            return EParseResult::Malformed;
        }

        const FStringView Method  = RequestLine.substr(0, FirstSpace);
        const FStringView Target  = RequestLine.substr(FirstSpace + 1, SecondSpace - FirstSpace - 1);
        const FStringView Version = RequestLine.substr(SecondSpace + 1);

        if (Method.empty() || Target.empty() || Version.substr(0, 5) != "HTTP/")
        {
            return EParseResult::Malformed;
        }

        FRequest Parsed;
        Parsed.Method = FString(Method.data(), Method.size());
        Parsed.Target = FString(Target.data(), Target.size());

        FStringView Rest = RequestLineEnd == FStringView::npos
            ? FStringView()
            : Head.substr(RequestLineEnd + 2);

        while (!Rest.empty())
        {
            const size_t LineEnd = Rest.find("\r\n");
            const FStringView Line = Rest.substr(0, LineEnd == FStringView::npos ? Rest.size() : LineEnd);

            const size_t Colon = Line.find(':');
            if (Colon == FStringView::npos)
            {
                return EParseResult::Malformed;
            }

            const FStringView Name  = TrimSpace(Line.substr(0, Colon));
            const FStringView Value = TrimSpace(Line.substr(Colon + 1));

            if (Name.empty())
            {
                return EParseResult::Malformed;
            }

            Parsed.Headers.push_back(FHeader{ FString(Name.data(), Name.size()), FString(Value.data(), Value.size()) });

            if (LineEnd == FStringView::npos)
            {
                break;
            }

            Rest = Rest.substr(LineEnd + 2);
        }

        // Chunked bodies are refused outright rather than half handled, since no client here needs them.
        const FStringView Encoding = Parsed.FindHeader("Transfer-Encoding");
        if (!Encoding.empty() && !EqualsIgnoringCase(Encoding, "identity"))
        {
            return EParseResult::Malformed;
        }

        int32 ContentLength = 0;
        const FStringView LengthText = Parsed.FindHeader("Content-Length");
        if (!LengthText.empty() && !ParseNonNegative(LengthText, ContentLength))
        {
            return EParseResult::Malformed;
        }

        if (ContentLength > Limits.MaxBodyBytes)
        {
            return EParseResult::Malformed;
        }

        const size_t BodyStart = HeaderEnd + GHeaderTerminator.size();
        if (View.size() - BodyStart < static_cast<size_t>(ContentLength))
        {
            return EParseResult::Incomplete;
        }

        Parsed.Body = FString(View.data() + BodyStart, static_cast<size_t>(ContentLength));

        // Whatever follows belongs to the next request on this connection and has to survive.
        Buffer = FString(View.data() + BodyStart + ContentLength,
                         View.size() - BodyStart - static_cast<size_t>(ContentLength));

        OutRequest = Move(Parsed);
        return EParseResult::Complete;
    }
}
