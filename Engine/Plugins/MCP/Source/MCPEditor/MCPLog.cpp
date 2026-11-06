#include "MCPLog.h"

#include "Containers/Algorithm.h"
#include "Containers/StringFormat.h"
#include "Core/Json/Json.h"
#include "GUID/GUID.h"

namespace Lumina::MCP
{
    namespace MCPLogInternal
    {
        constexpr int32 kMaxArgumentsBytes = 240;

        // A protocol failure carries an error, while a tool that ran and failed says so in its result.
        FString FindError(const FJsonValue& Reply)
        {
            if (const FJsonValue* Error = Reply.Find("error"))
            {
                return Error->GetString("message", "Unknown error");
            }

            const FJsonValue& Result = Reply["result"];
            if (!Result["isError"].AsBool())
            {
                return {};
            }

            const FJsonValue& Content = Result["content"];
            return Content.IsArray() && !Content.IsEmpty() ? Content[0].GetString("text", "Tool failed") : FString("Tool failed");
        }
    }

    using namespace MCPLogInternal;

    FString ClipText(FStringView Text, int32 MaxBytes)
    {
        if (Text.Len() <= MaxBytes)
        {
            return FString(Text);
        }

        int32 Kept = MaxBytes;
        while (Kept > 0 && (static_cast<uint8>(Text[Kept]) & 0xC0) == 0x80)
        {
            --Kept;
        }

        FString Clipped(Text.Left(Kept));
        Clipped += Format(" [{} more bytes]", Text.Len() - Kept);
        return Clipped;
    }

    FLog::FLog()
        : Transactions(kCapacity)
    {
    }

    FString FLog::Record(FStringView Session, FStringView Request, FStringView Response, double DurationMs)
    {
        TSharedPtr<FTransaction> Transaction = MakeShared<FTransaction>();
        Transaction->Time       = PlatformTime::LocalNow();
        Transaction->Session    = FString(Session);
        Transaction->Request    = ClipText(Request, kMaxPayloadBytes);
        Transaction->Response   = ClipText(Response, kMaxPayloadBytes);
        Transaction->DurationMs = DurationMs;

        FJsonValue Message;
        if (!FJsonValue::Parse(Request, Message))
        {
            Transaction->Method = "(unparsed)";
        }
        else if (Message.IsArray())
        {
            Transaction->Method = "(batch)";
        }
        else
        {
            Transaction->Method = Message.GetString("method");

            const FJsonValue& Params = Message["params"];
            const bool bToolCall = Transaction->Method == "tools/call";
            if (bToolCall)
            {
                Transaction->Tool = Params.GetString("name");
            }

            const FJsonValue& Shown = bToolCall ? Params["arguments"] : Params;
            if (!Shown.IsNull() && !Shown.IsEmpty())
            {
                Transaction->Arguments = ClipText(FStringView(Shown.ToString()), kMaxArgumentsBytes);
            }
        }

        FJsonValue Reply;
        if (!Response.IsEmpty() && FJsonValue::Parse(Response, Reply))
        {
            Transaction->Error = FindError(Reply);
        }

        FString Opened;
        if (Transaction->Method == "initialize" && !Transaction->IsError())
        {
            Opened = FGuid::New().ToString(false);
            Transaction->Session = Opened;
        }

        {
            TScopeLock Lock(Mutex);

            FConnection& Connection = FindOrAddConnection(FStringView(Transaction->Session), Transaction->Time);
            if (!Opened.IsEmpty())
            {
                const FJsonValue& Info = Message["params"]["clientInfo"];
                Connection.Client = Format("{} {}", Info.GetString("name", "Unnamed client"), Info.GetString("version"));
            }

            Connection.LastSeen = Transaction->Time;
            Connection.LastSeenRevision = Revision.fetch_add(1, std::memory_order_release) + 1;
            ++Connection.Requests;
            Connection.Errors += Transaction->IsError() ? 1 : 0;

            Transactions.Add(Move(Transaction));
        }

        return Opened;
    }

    void FLog::RecordDisconnect(FStringView Session)
    {
        TSharedPtr<FTransaction> Transaction = MakeShared<FTransaction>();
        Transaction->Time    = PlatformTime::LocalNow();
        Transaction->Session = FString(Session);
        Transaction->Method  = "(disconnect)";

        {
            TScopeLock Lock(Mutex);

            const uint64 Seen = Revision.fetch_add(1, std::memory_order_release) + 1;
            if (FConnection* Connection = FindConnection(Session))
            {
                Connection->LastSeen = Transaction->Time;
                Connection->LastSeenRevision = Seen;
                Connection->bOpen = false;
            }

            Transactions.Add(Move(Transaction));
        }
    }

    void FLog::Clear()
    {
        {
            TScopeLock Lock(Mutex);
            Transactions.Reset();
        }

        Revision.fetch_add(1, std::memory_order_release);
    }

    void FLog::Snapshot(TVector<TSharedPtr<const FTransaction>>& OutTransactions, TVector<FConnection>& OutConnections) const
    {
        TScopeLock Lock(Mutex);

        OutTransactions.Reset();
        OutTransactions.Reserve(Transactions.Num());
        for (int32 Index = 0; Index < Transactions.Num(); ++Index)
        {
            OutTransactions.Add(Transactions[Index]);
        }

        OutConnections = Connections;
    }

    FConnection* FLog::FindConnection(FStringView Session)
    {
        const auto Found = Algo::FindIf(Connections, [Session](const FConnection& Each) { return Each.Session == Session; });
        return Found != Connections.end() ? &*Found : nullptr;
    }

    FConnection& FLog::FindOrAddConnection(FStringView Session, const PlatformTime::FDateTime& Now)
    {
        if (FConnection* Found = FindConnection(Session))
        {
            return *Found;
        }

        if (Connections.Num() >= kMaxConnections)
        {
            const auto Evicted = Algo::MinElement(Connections, [](const FConnection& A, const FConnection& B)
            {
                return A.bOpen != B.bOpen ? !A.bOpen : A.LastSeenRevision < B.LastSeenRevision;
            });
            Connections.RemoveAt(static_cast<int32>(Evicted - Connections.begin()));
        }

        FConnection& Added = Connections.AddGetRef(FConnection());
        Added.Session   = FString(Session);
        Added.Client    = Session.IsEmpty() ? "No session" : "Unknown client";
        Added.FirstSeen = Now;
        return Added;
    }
}
