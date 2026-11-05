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
                *static_cast<FString*>(Context) = FStringView(Data, (size_t)Length);
            }
        }

        void SplitReply(FStringView Reply, TVector<TVector<FString>>& Out)
        {
            int32 RecordStart = 0;
            while (RecordStart < Reply.Len())
            {
                int32 RecordEnd = Reply.Find(RecordSeparator, RecordStart);
                RecordEnd = RecordEnd == Constants::kIndexNone ? Reply.Len() : RecordEnd;
                const FStringView Record = Reply.Mid(RecordStart, RecordEnd - RecordStart);
                TVector<FString>& Fields = Out.EmplaceGetRef();
                int32 FieldStart = 0;
                while (FieldStart <= Record.Len())
                {
                    int32 FieldEnd = Record.Find(FieldSeparator, FieldStart);
                    FieldEnd = FieldEnd == Constants::kIndexNone ? Record.Len() : FieldEnd;
                    Fields.Emplace(Record.Mid(FieldStart, FieldEnd - FieldStart));
                    FieldStart = FieldEnd + 1;
                }
                RecordStart = RecordEnd + 1;
            }
        }

        int32 ToInt(const FString& Text)
        {
            return (int32)std::strtol(*Text, nullptr, 10);
        }

        FString FieldOf(const TVector<FString>& Record, int32 Index)
        {
            return Record.IsValidIndex(Index) ? Record[Index] : FString();
        }
    }

    using namespace CSharpScriptLanguageInternal;

    FCSharpScriptLanguage::EReply FCSharpScriptLanguage::Query(EQuery Kind, const FCodeDocument& Document, FCodePosition Position, TVector<FRecord>& OutRecords) const
    {
        const FString Text = Document.GetText();
        return QueryText(Kind, Text.View(), (int32)Document.PositionToOffset(Position), OutRecords);
    }

    FCSharpScriptLanguage::EReply FCSharpScriptLanguage::QueryText(EQuery Kind, FStringView Text, int32 Offset, TVector<FRecord>& OutRecords) const
    {
        auto* Function = QueryExport.Get();
        if (Function == nullptr || FilePath.IsEmpty())
        {
            return EReply::Unavailable;
        }
        FString Reply;
        const int32 Result = Function((int32)Kind, *FilePath, FilePath.Len(), Text.GetData(), Text.Len(), Offset,
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
        if (Typed != '.' || CaretState != 0 || LinePrefix.Len() < 2)
        {
            return false;
        }
        int32 Start = LinePrefix.Len() - 1;
        while (Start > 0 && IsWordChar(LinePrefix[Start - 1]))
        {
            --Start;
        }
        const char Before = LinePrefix[LinePrefix.Len() - 2];
        const bool bNumber = Start < LinePrefix.Len() - 1 && CodeText::IsDigit(LinePrefix[Start]);
        return !bNumber && (IsWordChar(Before) || Before == ')' || Before == ']' || Before == '>');
    }

    void FCSharpScriptLanguage::GetCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const
    {
        const bool bMember = Request.WordStart.Column > 0 && Request.LinePrefix[Request.WordStart.Column - 1] == '.';
        if (!CanCompleteAt(Request) || (!bMember && Request.Word.IsEmpty() && !Request.bExplicit))
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
            if (Record.IsEmpty())
            {
                continue;
            }
            FCodeCompletionItem& Item = Out.EmplaceGetRef();
            Item.Label = Record[0];
            Item.Kind = (ECodeCompletionKind)ToInt(FieldOf(Record, 1));
            Item.Detail = FieldOf(Record, 2);
            Item.Documentation = FieldOf(Record, 3);
            Item.InsertText = FieldOf(Record, 4);
            Item.SortBoost = ToInt(FieldOf(Record, 5));
            Added.Add(Record[0].View());
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
        if (Records.IsEmpty() || FieldOf(Records[0], 0).IsEmpty())
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
            if (Record.Num() < 6)
            {
                continue;
            }
            FCodeDiagnostic& Diagnostic = Out.EmplaceGetRef();
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
        if (Records.IsEmpty() || FieldOf(Records[0], 0).IsEmpty())
        {
            return false;
        }
        const FRecord& Record = Records[0];
        Out.Label = FieldOf(Record, 0);
        Out.Documentation = FieldOf(Record, 1);
        Out.ActiveParameter = ToInt(FieldOf(Record, 2));
        Out.Parameters.Reset();
        const FStringView Spans = FieldOf(Record, 3).View();
        int32 Start = 0;
        while (Start < Spans.Len())
        {
            int32 End = Spans.Find(';', Start);
            End = End == Constants::kIndexNone ? Spans.Len() : End;
            const FString Span(Spans.Mid(Start, End - Start));
            const int32 Comma = Span.Find(',');
            if (Comma != Constants::kIndexNone)
            {
                Out.Parameters.Emplace(ToInt(Span), (int32)std::strtol(*Span + Comma + 1, nullptr, 10));
            }
            Start = End + 1;
        }
        return true;
    }

    bool FCSharpScriptLanguage::FindDefinition(const FCodeDocument& Document, FCodePosition Position, FCodeLocation& Out) const
    {
        TVector<FRecord> Records;
        if (Query(EQuery::Definition, Document, Position, Records) != EReply::Answered || Records.IsEmpty() || Records[0].Num() < 3)
        {
            return false;
        }
        const FRecord& Record = Records[0];
        Out.Path = Record[0];
        Out.Position = { ToInt(Record[1]), ToInt(Record[2]) };
        return true;
    }

    void FCSharpScriptLanguage::Sync(const FCodeDocument& Document) const
    {
        TVector<FRecord> Ignored;
        Query(EQuery::Sync, Document, {}, Ignored);
    }

    FCSharpScriptLanguage::EReply FCSharpScriptLanguage::FindReferences(const FCodeDocument& Document, FCodePosition Position, TVector<FReference>& Out) const
    {
        TVector<FRecord> Records;
        const EReply Reply = Query(EQuery::References, Document, Position, Records);
        for (const FRecord& Record : Records)
        {
            if (Record.Num() < 7)
            {
                continue;
            }
            FReference& Reference = Out.EmplaceGetRef();
            Reference.Path = Record[0];
            Reference.Range = { { ToInt(Record[1]), ToInt(Record[2]) }, { ToInt(Record[3]), ToInt(Record[4]) } };
            Reference.Preview = Record[5];
            Reference.bDeclaration = Record[6] == "1";
        }
        return Reply;
    }

    namespace CSharpScriptLanguageInternal
    {
        void ReadSymbols(const TVector<TVector<FString>>& Records, TVector<FCodeSymbolInfo>& Out)
        {
            for (const TVector<FString>& Record : Records)
            {
                if (Record.Num() < 9)
                {
                    continue;
                }
                FCodeSymbolInfo& Symbol = Out.EmplaceGetRef();
                Symbol.Name = Record[0];
                Symbol.Kind = (ECodeCompletionKind)ToInt(Record[1]);
                Symbol.Detail = Record[2];
                Symbol.Container = Record[3];
                Symbol.Path = Record[4];
                Symbol.Position = { ToInt(Record[5]), ToInt(Record[6]) };
                Symbol.EndLine = ToInt(Record[7]);
                Symbol.Depth = ToInt(Record[8]);
            }
        }
    }

    bool FCSharpScriptLanguage::GetDocumentSymbols(const FCodeDocument& Document, TVector<FCodeSymbolInfo>& Out) const
    {
        TVector<FRecord> Records;
        switch (Query(EQuery::DocumentSymbols, Document, {}, Records))
        {
            case EReply::Busy:
                return false;
            case EReply::Unavailable:
                return FCSharpLanguage::GetDocumentSymbols(Document, Out);
            default:
                break;
        }
        ReadSymbols(Records, Out);
        for (FCodeSymbolInfo& Symbol : Out)
        {
            Symbol.Path.Reset();
        }
        return true;
    }

    FCSharpScriptLanguage::EReply FCSharpScriptLanguage::FindWorkspaceSymbols(FStringView Search, TVector<FCodeSymbolInfo>& Out) const
    {
        TVector<FRecord> Records;
        const EReply Reply = QueryText(EQuery::WorkspaceSymbols, Search, 0, Records);
        ReadSymbols(Records, Out);
        return Reply;
    }

    FCSharpScriptLanguage::EReply FCSharpScriptLanguage::GetProjectDiagnostics(TVector<FProjectDiagnostic>& Out) const
    {
        TVector<FRecord> Records;
        const EReply Reply = QueryText(EQuery::ProjectDiagnostics, {}, 0, Records);
        for (const FRecord& Record : Records)
        {
            if (Record.Num() < 7)
            {
                continue;
            }
            FProjectDiagnostic& Problem = Out.EmplaceGetRef();
            Problem.Path = Record[0];
            Problem.Diagnostic.Range = { { ToInt(Record[1]), ToInt(Record[2]) }, { ToInt(Record[3]), ToInt(Record[4]) } };
            Problem.Diagnostic.Severity = (ECodeSeverity)Math::Clamp(ToInt(Record[5]), 0, 2);
            Problem.Diagnostic.Message = Record[6];
        }
        return Reply;
    }

    void FCSharpScriptLanguage::GetCodeActions(const FCodeDocument& Document, FCodePosition Position, TVector<FCodeAction>& Out) const
    {
        TVector<FRecord> Records;
        if (Query(EQuery::CodeActions, Document, Position, Records) != EReply::Answered)
        {
            return;
        }
        for (const FRecord& Record : Records)
        {
            if (Record.Num() < 7)
            {
                continue;
            }
            FCodeAction& Action = Out.EmplaceGetRef();
            Action.Title = Record[0];
            FCodeTextEdit& Edit = Action.Edits.EmplaceGetRef();
            // An edit to the script being asked about stays in the editor rather than going to the file on disk.
            Edit.Path = Record[1] == FilePath ? FString() : Record[1];
            Edit.Range = { { ToInt(Record[2]), ToInt(Record[3]) }, { ToInt(Record[4]), ToInt(Record[5]) } };
            Edit.Text = Record[6];
        }
    }

    FString CSharpScript::NewScriptSource(FStringView FileStem)
    {
        FString ClassName;
        for (const char C : FileStem)
        {
            ClassName.AppendChar(CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '_' ? C : '_');
        }
        if (ClassName.IsEmpty() || CodeText::IsDigit(ClassName[0]))
        {
            ClassName.InsertAt(0, '_');
        }

        FString Source;
        Source += "using LuminaSharp;\n";
        Source += "using Lumina;\n\n";
        // Not "Game", which would shadow LuminaSharp.Game and break Game.OpenLevel in every script.
        Source += "namespace GameScripts;\n\n";
        Source += "[Reflect]\n";
        Source += "public sealed class " + ClassName + " : CEntityScript\n";
        Source += "{\n";
        Source += "    public override void OnReady()\n";
        Source += "    {\n";
        Source += "    }\n\n";
        Source += "    public override void OnUpdate(float deltaTime)\n";
        Source += "    {\n";
        Source += "    }\n";
        Source += "}\n";
        return Source;
    }
}
