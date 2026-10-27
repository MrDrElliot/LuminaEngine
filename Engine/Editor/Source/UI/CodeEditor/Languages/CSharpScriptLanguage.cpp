#include "CSharpScriptLanguage.h"

#include <cstdlib>

namespace Lumina
{
    namespace CSharpScriptLanguageInternal
    {
        constexpr char FieldSeparator = '\x1F';
        constexpr char RecordSeparator = '\x1E';

        void ReceiveReply(void* Context, const char* Data, int32 Length)
        {
            if (Data != nullptr && Length > 0)
            {
                static_cast<FString*>(Context)->assign(Data, (size_t)Length);
            }
        }

        void SplitReply(FStringView Reply, TVector<TVector<FString>>& Out)
        {
            size_t RecordStart = 0;
            while (RecordStart < Reply.size())
            {
                size_t RecordEnd = Reply.find(RecordSeparator, RecordStart);
                RecordEnd = RecordEnd == FStringView::npos ? Reply.size() : RecordEnd;
                const FStringView Record = Reply.substr(RecordStart, RecordEnd - RecordStart);
                TVector<FString>& Fields = Out.emplace_back();
                size_t FieldStart = 0;
                while (FieldStart <= Record.size())
                {
                    size_t FieldEnd = Record.find(FieldSeparator, FieldStart);
                    FieldEnd = FieldEnd == FStringView::npos ? Record.size() : FieldEnd;
                    Fields.emplace_back(Record.substr(FieldStart, FieldEnd - FieldStart));
                    FieldStart = FieldEnd + 1;
                }
                RecordStart = RecordEnd + 1;
            }
        }

        int32 ToInt(const FString& Text)
        {
            return (int32)std::strtol(Text.c_str(), nullptr, 10);
        }

        FString FieldOf(const TVector<FString>& Record, size_t Index)
        {
            return Index < Record.size() ? Record[Index] : FString();
        }
    }

    using namespace CSharpScriptLanguageInternal;

    FCSharpScriptLanguage::EReply FCSharpScriptLanguage::Query(EQuery Kind, const FCodeDocument& Document, FCodePosition Position, TVector<FRecord>& OutRecords) const
    {
        auto* Function = QueryExport.Get();
        if (Function == nullptr || FilePath.empty())
        {
            return EReply::Unavailable;
        }
        const FString Text = Document.GetText();
        const int32 Offset = (int32)Document.PositionToOffset(Position);
        FString Reply;
        const int32 Result = Function((int32)Kind, FilePath.c_str(), (int32)FilePath.size(), Text.c_str(), (int32)Text.size(), Offset,
            reinterpret_cast<void*>(&ReceiveReply), &Reply);
        if (Result == 2)
        {
            return EReply::Busy;
        }
        if (Result != 0)
        {
            return EReply::Unavailable;
        }
        SplitReply(Reply.View(), OutRecords);
        return EReply::Answered;
    }

    void FCSharpScriptLanguage::Warm(const FCodeDocument& Document) const
    {
        TVector<FRecord> Ignored;
        Query(EQuery::Warm, Document, {}, Ignored);
    }

    bool FCSharpScriptLanguage::IsCompletionTrigger(char Typed, FStringView LinePrefix, uint32 CaretState) const
    {
        if (Typed != '.' || CaretState != 0 || LinePrefix.size() < 2)
        {
            return false;
        }
        int32 Start = (int32)LinePrefix.size() - 1;
        while (Start > 0 && IsWordChar(LinePrefix[Start - 1]))
        {
            --Start;
        }
        const char Before = LinePrefix[LinePrefix.size() - 2];
        const bool bNumber = Start < (int32)LinePrefix.size() - 1 && CodeText::IsDigit(LinePrefix[Start]);
        return !bNumber && (IsWordChar(Before) || Before == ')' || Before == ']' || Before == '>');
    }

    void FCSharpScriptLanguage::GetCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const
    {
        const bool bMember = Request.WordStart.Column > 0 && Request.LinePrefix[Request.WordStart.Column - 1] == '.';
        if (!CanCompleteAt(Request) || (!bMember && Request.Word.empty() && !Request.bExplicit))
        {
            return;
        }

        TVector<FRecord> Records;
        if (Query(EQuery::Completion, *Request.Document, Request.Caret, Records) != EReply::Answered)
        {
            if (!bMember)
            {
                FCSharpLanguage::GetCompletions(Request, Out);
            }
            return;
        }

        // Keyed on the reply records, which stay put while Out grows.
        THashSet<FStringView> Added;
        for (const FRecord& Record : Records)
        {
            if (Record.empty())
            {
                continue;
            }
            FCodeCompletionItem& Item = Out.emplace_back();
            Item.Label = Record[0];
            Item.Kind = (ECodeCompletionKind)ToInt(FieldOf(Record, 1));
            Item.Detail = FieldOf(Record, 2);
            Item.Documentation = FieldOf(Record, 3);
            Item.InsertText = FieldOf(Record, 4);
            Item.SortBoost = ToInt(FieldOf(Record, 5));
            Added.insert(Record[0].View());
        }
        if (!bMember)
        {
            AddKeywordsAndSnippets(Out, Added);
        }
    }

    bool FCSharpScriptLanguage::GetHover(const FCodeDocument& Document, FCodePosition Position, FCodeHover& Out) const
    {
        TVector<FRecord> Records;
        const EReply Reply = Query(EQuery::Hover, Document, Position, Records);
        if (Reply == EReply::Unavailable)
        {
            return FCSharpLanguage::GetHover(Document, Position, Out);
        }
        if (Records.empty() || FieldOf(Records[0], 0).empty())
        {
            return false;
        }
        const FRecord& Record = Records[0];
        Out.Title = FieldOf(Record, 0);
        Out.Body = FieldOf(Record, 1);
        Out.Range = { { ToInt(FieldOf(Record, 2)), ToInt(FieldOf(Record, 3)) }, { ToInt(FieldOf(Record, 4)), ToInt(FieldOf(Record, 5)) } };
        return true;
    }

    bool FCSharpScriptLanguage::GetDiagnostics(const FCodeDocument& Document, TVector<FCodeDiagnostic>& Out) const
    {
        TVector<FRecord> Records;
        switch (Query(EQuery::Diagnostics, Document, {}, Records))
        {
            case EReply::Busy:
                return false;
            case EReply::Unavailable:
                return FCSharpLanguage::GetDiagnostics(Document, Out);
            default:
                break;
        }
        for (const FRecord& Record : Records)
        {
            if (Record.size() < 6)
            {
                continue;
            }
            FCodeDiagnostic& Diagnostic = Out.emplace_back();
            Diagnostic.Range = { { ToInt(Record[0]), ToInt(Record[1]) }, { ToInt(Record[2]), ToInt(Record[3]) } };
            Diagnostic.Severity = (ECodeSeverity)Math::Clamp(ToInt(Record[4]), 0, 2);
            Diagnostic.Message = Record[5];
        }
        return true;
    }

    bool FCSharpScriptLanguage::GetSignature(const FCodeDocument& Document, FCodePosition Caret, FCodeSignature& Out) const
    {
        TVector<FRecord> Records;
        const EReply Reply = Query(EQuery::Signature, Document, Caret, Records);
        if (Reply == EReply::Unavailable)
        {
            return FCSharpLanguage::GetSignature(Document, Caret, Out);
        }
        if (Records.empty() || FieldOf(Records[0], 0).empty())
        {
            return false;
        }
        const FRecord& Record = Records[0];
        Out.Label = FieldOf(Record, 0);
        Out.Documentation = FieldOf(Record, 1);
        Out.ActiveParameter = ToInt(FieldOf(Record, 2));
        Out.Parameters.clear();
        const FStringView Spans = FieldOf(Record, 3).View();
        size_t Start = 0;
        while (Start < Spans.size())
        {
            size_t End = Spans.find(';', Start);
            End = End == FStringView::npos ? Spans.size() : End;
            const FString Span(Spans.substr(Start, End - Start));
            const size_t Comma = Span.find(',');
            if (Comma != FString::npos)
            {
                Out.Parameters.push_back({ ToInt(Span), (int32)std::strtol(Span.c_str() + Comma + 1, nullptr, 10) });
            }
            Start = End + 1;
        }
        return true;
    }

    bool FCSharpScriptLanguage::FindDefinition(const FCodeDocument& Document, FCodePosition Position, FCodeLocation& Out) const
    {
        TVector<FRecord> Records;
        if (Query(EQuery::Definition, Document, Position, Records) != EReply::Answered || Records.empty() || Records[0].size() < 3)
        {
            return false;
        }
        const FRecord& Record = Records[0];
        Out.Path = Record[0];
        Out.Position = { ToInt(Record[1]), ToInt(Record[2]) };
        return true;
    }
}
