#include "MCPLog.h"

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

        FString Clipped(Text.Left(MaxBytes));
        Clipped += Format(" [{} more bytes]", Text.Len() - MaxBytes);
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
            ++Connection.Requests;
            Connection.Errors += Transaction->IsError() ? 1 : 0;

            Transactions.Add(Move(Transaction));
        }

        Revision.fetch_add(1, std::memory_order_release);
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

            FConnection& Connection = FindOrAddConnection(Session, Transaction->Time);
            Connection.LastSeen = Transaction->Time;
            Connection.bOpen = false;

            Transactions.Add(Move(Transaction));
        }

        Revision.fetch_add(1, std::memory_order_release);
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

    FConnection& FLog::FindOrAddConnection(FStringView Session, const PlatformTime::FDateTime& Now)
    {
        for (FConnection& Connection : Connections)
        {
            if (Connection.Session == Session)
            {
                return Connection;
            }
        }

        if (Connections.Num() >= kMaxConnections)
        {
            Connections.RemoveAt(0);
        }

        FConnection& Added = Connections.AddGetRef(FConnection());
        Added.Session   = FString(Session);
        Added.Client    = Session.IsEmpty() ? "No session" : "Unknown client";
        Added.FirstSeen = Now;
        return Added;
    }
}
