#include "CodeEditor.h"

#include <chrono>
#include "UI/Tools/NodeGraph/GraphActionMenu.h"

namespace Lumina
{
    namespace CodeEditorInternal
    {
        constexpr double TypingMergeSeconds = 1.0;
        constexpr int32  MaxUndoRecords = 1000;
        constexpr int32  MaxFindMatches = 10000;
        constexpr double DiagnosticsDelay = 0.3;

        void AdjustForInsert(FCodePosition& Position, FCodePosition At, FCodePosition End, bool bStickLeft)
        {
            if (Position < At || (bStickLeft && Position == At))
            {
                return;
            }
            if (Position.Line == At.Line)
            {
                Position.Column = End.Column + (Position.Column - At.Column);
                Position.Line = End.Line;
            }
            else
            {
                Position.Line += End.Line - At.Line;
            }
        }

        void AdjustForErase(FCodePosition& Position, const FCodeRange& Range)
        {
            if (Position <= Range.Start)
            {
                return;
            }
            if (Position < Range.End)
            {
                Position = Range.Start;
            }
            else if (Position.Line == Range.End.Line)
            {
                Position.Column = Range.Start.Column + (Position.Column - Range.End.Column);
                Position.Line = Range.Start.Line;
            }
            else
            {
                Position.Line -= Range.End.Line - Range.Start.Line;
            }
        }

        // Walks Text from At to the byte at Offset, so a snippet's tab stops land where its text went.
        FCodePosition AdvanceThrough(FCodePosition At, FStringView Text, size_t Offset)
        {
            for (size_t i = 0; i < Offset && i < Text.size(); ++i)
            {
                if (Text[i] == '\n')
                {
                    ++At.Line;
                    At.Column = 0;
                }
                else
                {
                    ++At.Column;
                }
            }
            return At;
        }

        bool SameCursors(const auto& A, const auto& B)
        {
            if (A.size() != B.size())
            {
                return false;
            }
            for (size_t i = 0; i < A.size(); ++i)
            {
                if (!(A[i] == B[i]))
                {
                    return false;
                }
            }
            return true;
        }

        int32 CharClass(char C, bool bWord)
        {
            if (CodeText::IsSpace(C))
            {
                return 0;
            }
            return bWord ? 1 : 2;
        }

        void EncodeUtf8(uint32 Codepoint, char* Out)
        {
            if (Codepoint < 0x80)
            {
                Out[0] = (char)Codepoint;
                Out[1] = 0;
            }
            else if (Codepoint < 0x800)
            {
                Out[0] = (char)(0xC0 | (Codepoint >> 6));
                Out[1] = (char)(0x80 | (Codepoint & 0x3F));
                Out[2] = 0;
            }
            else if (Codepoint < 0x10000)
            {
                Out[0] = (char)(0xE0 | (Codepoint >> 12));
                Out[1] = (char)(0x80 | ((Codepoint >> 6) & 0x3F));
                Out[2] = (char)(0x80 | (Codepoint & 0x3F));
                Out[3] = 0;
            }
            else
            {
                Out[0] = (char)(0xF0 | (Codepoint >> 18));
                Out[1] = (char)(0x80 | ((Codepoint >> 12) & 0x3F));
                Out[2] = (char)(0x80 | ((Codepoint >> 6) & 0x3F));
                Out[3] = (char)(0x80 | (Codepoint & 0x3F));
                Out[4] = 0;
            }
        }
    }

    using namespace CodeEditorInternal;

    double FCodeEditor::Now()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    FCodeEditor::FCodeEditor()
    {
        Cursors.emplace_back();
        Palette = FCodeEditorPalette::Dark();
    }

    void FCodeEditor::SetText(FStringView Text)
    {
        Document.SetText(Text);
        ResetEditState();
    }

    void FCodeEditor::ResetEditState()
    {
        Cursors.clear();
        Cursors.emplace_back();
        MainCursor = 0;
        UndoStack.clear();
        RedoStack.clear();
        PendingEdit = {};
        SnippetStops.clear();
        SnippetCurrent = -1;
        Completion.bOpen = false;
        bSignatureOpen = false;
        LanguageDiagnostics.clear();
        DiagnosticsVersion = ~0ull;
        Find.MatchesVersion = ~0ull;
        ReportedVersion = Document.GetVersion();
        bChangePending = false;
        ColorPick.bRequestOpen = false;
    }

    FString FCodeEditor::GetSelectedText() const
    {
        return Document.GetRangeText(Cursors[MainCursor].GetRange());
    }

    void FCodeEditor::SetLanguage(const FCodeLanguage* InLanguage)
    {
        Language = InLanguage;
        InvalidateHighlight();
    }

    void FCodeEditor::InvalidateHighlight()
    {
        Document.InvalidateHighlight();
        DiagnosticsVersion = ~0ull;
    }

    void FCodeEditor::SetChangeCallback(TFunction<void()> Callback, float DelaySeconds)
    {
        ChangeCallback = Move(Callback);
        ChangeDelay = DelaySeconds;
        ReportedVersion = Document.GetVersion();
    }

    void FCodeEditor::SetExternalDiagnostics(TVector<FCodeDiagnostic> Diagnostics)
    {
        ExternalDiagnostics = Move(Diagnostics);
    }

    FCodeRange FCodeEditor::GetSelection() const
    {
        return Cursors[MainCursor].GetRange();
    }

    bool FCodeEditor::HasSelection() const
    {
        return Algo::AnyOf(Cursors.begin(), Cursors.end(), [](const FCursor& Cursor) { return Cursor.HasSelection(); });
    }

    void FCodeEditor::SetCursor(FCodePosition Position, bool bCenter)
    {
        Position = Document.Clamp(Position);
        Select({ Position, Position }, bCenter);
    }

    void FCodeEditor::Select(const FCodeRange& Range, bool bCenter)
    {
        ClearExtraCursors();
        Cursors[0].Anchor = Document.Clamp(Range.Start);
        Cursors[0].Head = Document.Clamp(Range.End);
        Cursors[0].PreferredVisual = -1;
        if (bCenter)
        {
            CenterLineRequest = Cursors[0].Head.Line;
        }
        else
        {
            EnsureCursorVisible();
        }
        OnCursorsMoved();
    }

    void FCodeEditor::SelectAll()
    {
        ClearExtraCursors();
        Cursors[0].Anchor = {};
        Cursors[0].Head = Document.GetEnd();
        OnCursorsMoved();
    }

    void FCodeEditor::ScrollToLine(int32 Line)
    {
        CenterLineRequest = Math::Clamp(Line, 0, Document.GetLineCount() - 1);
    }

    FCodePosition FCodeEditor::InsertRaw(FCodePosition At, FStringView Text)
    {
        At = Document.Clamp(At);
        const FCodePosition End = Document.Insert(At, Text);
        if (End == At)
        {
            return At;
        }
        if (!bReplaying)
        {
            FEditOp& Op = PendingEdit.Ops.emplace_back();
            Op.Start = At;
            Op.End = End;
            Op.Text = Text.find('\r') == FStringView::npos ? FString(Text) : Document.GetRangeText({ At, End });
            Op.bInsert = true;
        }
        for (FCursor& Cursor : Cursors)
        {
            AdjustForInsert(Cursor.Anchor, At, End, false);
            AdjustForInsert(Cursor.Head, At, End, false);
        }
        for (FSnippetStop& Stop : SnippetStops)
        {
            AdjustForInsert(Stop.Range.Start, At, End, true);
            AdjustForInsert(Stop.Range.End, At, End, false);
        }
        AdjustForInsert(Completion.WordStart, At, End, true);
        LastEditTime = Now();
        return End;
    }

    void FCodeEditor::EraseRaw(const FCodeRange& InRange)
    {
        FCodeRange Range{ Document.Clamp(InRange.Start), Document.Clamp(InRange.End) };
        if (Range.End < Range.Start)
        {
            std::swap(Range.Start, Range.End);
        }
        if (Range.IsEmpty())
        {
            return;
        }
        if (!bReplaying)
        {
            FEditOp& Op = PendingEdit.Ops.emplace_back();
            Op.Start = Range.Start;
            Op.End = Range.End;
            Op.Text = Document.GetRangeText(Range);
            Op.bInsert = false;
        }
        Document.Erase(Range);
        for (FCursor& Cursor : Cursors)
        {
            AdjustForErase(Cursor.Anchor, Range);
            AdjustForErase(Cursor.Head, Range);
        }
        for (FSnippetStop& Stop : SnippetStops)
        {
            AdjustForErase(Stop.Range.Start, Range);
            AdjustForErase(Stop.Range.End, Range);
        }
        AdjustForErase(Completion.WordStart, Range);
        LastEditTime = Now();
    }

    void FCodeEditor::ReplaceRange(const FCodeRange& Range, FStringView Text)
    {
        EraseRaw(Range);
        InsertRaw(Range.Start < Range.End ? Range.Start : Range.End, Text);
    }

    void FCodeEditor::BeginEdit(EEditKind Kind)
    {
        if (EditDepth++ == 0)
        {
            PendingEdit = {};
            PendingEdit.Kind = Kind;
            PendingEdit.Before = Cursors;
        }
    }

    void FCodeEditor::EndEdit()
    {
        if (--EditDepth > 0 || PendingEdit.Ops.empty())
        {
            return;
        }

        NormalizeCursors();
        PendingEdit.After = Cursors;
        PendingEdit.Time = Now();
        RedoStack.clear();

        bool bMerged = false;
        if (!UndoStack.empty() && PendingEdit.Kind != EEditKind::Other)
        {
            FUndoRecord& Top = UndoStack.back();
            if (Top.Kind == PendingEdit.Kind && PendingEdit.Time - Top.Time < TypingMergeSeconds && SameCursors(Top.After, PendingEdit.Before))
            {
                Top.Ops.insert(Top.Ops.end(), std::make_move_iterator(PendingEdit.Ops.begin()), std::make_move_iterator(PendingEdit.Ops.end()));
                Top.After = PendingEdit.After;
                Top.Time = PendingEdit.Time;
                bMerged = true;
            }
        }
        if (!bMerged)
        {
            UndoStack.push_back(Move(PendingEdit));
            if ((int32)UndoStack.size() > MaxUndoRecords)
            {
                UndoStack.erase(UndoStack.begin());
            }
        }
        PendingEdit = {};
        EnsureCursorVisible();
        OnCursorsMoved();
    }

    void FCodeEditor::ApplyUndoRecord(const FUndoRecord& Record, bool bUndo)
    {
        bReplaying = true;
        if (bUndo)
        {
            for (int32 i = (int32)Record.Ops.size() - 1; i >= 0; --i)
            {
                const FEditOp& Op = Record.Ops[i];
                if (Op.bInsert)
                {
                    EraseRaw({ Op.Start, Op.End });
                }
                else
                {
                    InsertRaw(Op.Start, Op.Text);
                }
            }
        }
        else
        {
            for (const FEditOp& Op : Record.Ops)
            {
                if (Op.bInsert)
                {
                    InsertRaw(Op.Start, Op.Text);
                }
                else
                {
                    EraseRaw({ Op.Start, Op.End });
                }
            }
        }
        bReplaying = false;

        Cursors = bUndo ? Record.Before : Record.After;
        MainCursor = 0;
        NormalizeCursors();
        SnippetStops.clear();
        SnippetCurrent = -1;
        CloseCompletion();
        EnsureCursorVisible();
        OnCursorsMoved();
    }

    void FCodeEditor::Undo()
    {
        if (!CanUndo())
        {
            return;
        }
        FUndoRecord Record = Move(UndoStack.back());
        UndoStack.pop_back();
        ApplyUndoRecord(Record, true);
        RedoStack.push_back(Move(Record));
    }

    void FCodeEditor::Redo()
    {
        if (!CanRedo())
        {
            return;
        }
        FUndoRecord Record = Move(RedoStack.back());
        RedoStack.pop_back();
        ApplyUndoRecord(Record, false);
        UndoStack.push_back(Move(Record));
    }

    void FCodeEditor::NormalizeCursors()
    {
        for (FCursor& Cursor : Cursors)
        {
            Cursor.Anchor = Document.Clamp(Cursor.Anchor);
            Cursor.Head = Document.Clamp(Cursor.Head);
        }
        MainCursor = Math::Clamp(MainCursor, 0, (int32)Cursors.size() - 1);
        if (Cursors.size() == 1)
        {
            return;
        }

        const FCodePosition MainHead = Cursors[MainCursor].Head;
        Algo::Sort(Cursors.begin(), Cursors.end(), [](const FCursor& A, const FCursor& B)
        {
            return A.GetRange().Start < B.GetRange().Start;
        });

        TVector<FCursor> Merged;
        Merged.reserve(Cursors.size());
        for (const FCursor& Cursor : Cursors)
        {
            if (!Merged.empty())
            {
                FCursor& Last = Merged.back();
                const FCodeRange Previous = Last.GetRange();
                const FCodeRange Current = Cursor.GetRange();
                const bool bTouching = Current.Start == Previous.End && (Current.IsEmpty() || Previous.IsEmpty());
                if (Current.Start < Previous.End || Current.Start == Previous.Start || bTouching)
                {
                    const FCodePosition End = Previous.End < Current.End ? Current.End : Previous.End;
                    Last.Anchor = Previous.Start;
                    Last.Head = End;
                    continue;
                }
            }
            Merged.push_back(Cursor);
        }
        Cursors = Move(Merged);

        MainCursor = (int32)Cursors.size() - 1;
        for (int32 i = 0; i < (int32)Cursors.size(); ++i)
        {
            const FCodeRange Range = Cursors[i].GetRange();
            if (Range.Start <= MainHead && MainHead <= Range.End)
            {
                MainCursor = i;
                break;
            }
        }
    }

    void FCodeEditor::ClearExtraCursors()
    {
        if (Cursors.size() > 1)
        {
            const FCursor Main = Cursors[MainCursor];
            Cursors.clear();
            Cursors.push_back(Main);
        }
        MainCursor = 0;
    }

    void FCodeEditor::OnCursorsMoved()
    {
        CursorBlinkStart = Now();
        if (bSignatureOpen)
        {
            RefreshSignature();
        }
        if (SnippetCurrent >= 0)
        {
            const FCodePosition Head = Cursors[MainCursor].Head;
            const bool bInside = Algo::AnyOf(SnippetStops.begin(), SnippetStops.end(), [Head](const FSnippetStop& Stop)
            {
                return Stop.Range.Start.Line <= Head.Line && Head.Line <= Stop.Range.End.Line;
            });
            if (!bInside)
            {
                SnippetStops.clear();
                SnippetCurrent = -1;
            }
        }
    }

    FCodeRange FCodeEditor::WordAt(FCodePosition Position) const
    {
        Position = Document.Clamp(Position);
        const FStringView Text = Document.GetLineView(Position.Line);
        auto IsWord = [this](char C)
        {
            return Language != nullptr ? Language->IsWordChar(C) : (CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '_');
        };
        int32 Start = Position.Column;
        int32 End = Position.Column;
        while (Start > 0 && IsWord(Text[Start - 1]))
        {
            --Start;
        }
        while (End < (int32)Text.size() && IsWord(Text[End]))
        {
            ++End;
        }
        return { { Position.Line, Start }, { Position.Line, End } };
    }

    FCodePosition FCodeEditor::WordBoundary(FCodePosition Position, bool bForward) const
    {
        auto IsWord = [this](char C)
        {
            return Language != nullptr ? Language->IsWordChar(C) : (CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '_');
        };
        const FStringView Text = Document.GetLineView(Position.Line);
        int32 Column = Position.Column;
        if (bForward)
        {
            if (Column >= (int32)Text.size())
            {
                return Document.Next(Position);
            }
            while (Column < (int32)Text.size() && CodeText::IsSpace(Text[Column]))
            {
                ++Column;
            }
            if (Column < (int32)Text.size())
            {
                const int32 Class = CharClass(Text[Column], IsWord(Text[Column]));
                while (Column < (int32)Text.size() && CharClass(Text[Column], IsWord(Text[Column])) == Class)
                {
                    ++Column;
                }
            }
        }
        else
        {
            if (Column <= 0)
            {
                return Document.Prev(Position);
            }
            while (Column > 0 && CodeText::IsSpace(Text[Column - 1]))
            {
                --Column;
            }
            if (Column > 0)
            {
                const int32 Class = CharClass(Text[Column - 1], IsWord(Text[Column - 1]));
                while (Column > 0 && CharClass(Text[Column - 1], IsWord(Text[Column - 1])) == Class)
                {
                    --Column;
                }
            }
        }
        return Document.Clamp({ Position.Line, Column });
    }

    FCodePosition FCodeEditor::MovePosition(FCodePosition Position, EMove Move, int32& PreferredVisual) const
    {
        const int32 TabSize = Math::Max(1, Settings.TabSize);
        const bool bVertical = Move == EMove::Up || Move == EMove::Down || Move == EMove::PageUp || Move == EMove::PageDown;
        if (!bVertical)
        {
            PreferredVisual = -1;
        }

        switch (Move)
        {
            case EMove::Left:
                return Document.Prev(Position);
            case EMove::Right:
                return Document.Next(Position);
            case EMove::WordLeft:
                return WordBoundary(Position, false);
            case EMove::WordRight:
                return WordBoundary(Position, true);
            case EMove::LineStart:
            {
                const int32 Indent = CodeText::LeadingWhitespace(Document.GetLineView(Position.Line));
                return { Position.Line, Position.Column == Indent ? 0 : Indent };
            }
            case EMove::LineEnd:
                return { Position.Line, (int32)Document.GetLine(Position.Line).size() };
            case EMove::DocumentStart:
                return {};
            case EMove::DocumentEnd:
                return Document.GetEnd();
            default:
                break;
        }

        const int32 Page = Layout.LineHeight > 0.0f ? Math::Max(1, (int32)((Layout.ViewMax.y - Layout.ViewMin.y) / Layout.LineHeight) - 2) : 20;
        const int32 Delta = Move == EMove::Up ? -1 : Move == EMove::Down ? 1 : Move == EMove::PageUp ? -Page : Page;
        if (PreferredVisual < 0)
        {
            PreferredVisual = CodeText::VisualColumn(Document.GetLineView(Position.Line), Position.Column, TabSize);
        }
        const int32 Target = Position.Line + Delta;
        if (Target < 0)
        {
            return {};
        }
        if (Target >= Document.GetLineCount())
        {
            return Document.GetEnd();
        }
        return { Target, CodeText::ColumnAtVisual(Document.GetLineView(Target), PreferredVisual, TabSize, false) };
    }

    void FCodeEditor::MoveCursors(EMove Move, bool bSelect)
    {
        for (FCursor& Cursor : Cursors)
        {
            if (!bSelect && Cursor.HasSelection() && (Move == EMove::Left || Move == EMove::Right))
            {
                const FCodeRange Range = Cursor.GetRange();
                Cursor.Head = Move == EMove::Left ? Range.Start : Range.End;
                Cursor.Anchor = Cursor.Head;
                Cursor.PreferredVisual = -1;
                continue;
            }
            Cursor.Head = MovePosition(Cursor.Head, Move, Cursor.PreferredVisual);
            if (!bSelect)
            {
                Cursor.Anchor = Cursor.Head;
            }
        }
        NormalizeCursors();
        EnsureCursorVisible();
        OnCursorsMoved();
    }

    TVector<int32> FCodeEditor::SelectedLines() const
    {
        TVector<int32> Lines;
        for (const FCursor& Cursor : Cursors)
        {
            const FCodeRange Range = Cursor.GetRange();
            int32 Last = Range.End.Line;
            if (Range.End.Line > Range.Start.Line && Range.End.Column == 0)
            {
                --Last;
            }
            for (int32 Line = Range.Start.Line; Line <= Last; ++Line)
            {
                Lines.push_back(Line);
            }
        }
        Algo::Sort(Lines.begin(), Lines.end());
        Lines.erase(Algo::Unique(Lines.begin(), Lines.end()), Lines.end());
        return Lines;
    }

    FString FCodeEditor::IndentUnit() const
    {
        return Settings.bInsertSpaces ? FString((size_t)Math::Max(1, Settings.TabSize), ' ') : FString("\t");
    }

    FString FCodeEditor::LineIndent(int32 Line) const
    {
        const FStringView Text = Document.GetLineView(Line);
        return FString(Text.substr(0, CodeText::LeadingWhitespace(Text)));
    }

    uint32 FCodeEditor::StateAt(FCodePosition Position)
    {
        if (Language == nullptr)
        {
            return 0;
        }
        Position = Document.Clamp(Position);
        Document.Highlight(*Language, Position.Line - 1);
        const uint32 Start = Document.GetStartState(*Language, Position.Line);
        if (Position.Column == 0)
        {
            return Start;
        }
        TVector<ECodeToken> Scratch;
        Scratch.resize(Position.Column);
        return Language->Tokenize(Document.GetLineView(Position.Line).substr(0, Position.Column), Start, Scratch.data());
    }

    void FCodeEditor::DeleteSelections()
    {
        for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
        {
            if (Cursors[i].HasSelection())
            {
                EraseRaw(Cursors[i].GetRange());
            }
        }
    }

    void FCodeEditor::TypeAtCursors(FStringView Text)
    {
        DeleteSelections();
        for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
        {
            InsertRaw(Cursors[i].Head, Text);
            Cursors[i].PreferredVisual = -1;
        }
    }

    void FCodeEditor::InsertText(FStringView Text)
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        BeginEdit(EEditKind::Other);
        TypeAtCursors(Text);
        EndEdit();
    }

    void FCodeEditor::TypeCharacter(uint32 Codepoint)
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        if (Codepoint == '\n' || Codepoint == '\r')
        {
            NewLine();
            return;
        }
        if (Codepoint < 32 && Codepoint != '\t')
        {
            return;
        }

        char Encoded[5] = {};
        EncodeUtf8(Codepoint, Encoded);
        const char Typed = Codepoint < 128 ? (char)Codepoint : 0;
        const bool bWhitespace = Typed == ' ' || Typed == '\t';

        BeginEdit(bWhitespace ? EEditKind::Other : EEditKind::Typing);
        const bool bHandled = Typed != 0 && (TryOvertypeCloser(Typed) || TryAutoClosePair(Typed));
        if (!bHandled)
        {
            TypeAtCursors(Encoded);
            if (Typed != 0)
            {
                OutdentLineIfTriggered(Typed);
                ApplyAutoInsert(Typed);
            }
        }
        EndEdit();
        MaybeTriggerAfterTyping(Typed != 0 ? Typed : 'a');
    }

    bool FCodeEditor::TryOvertypeCloser(char Typed)
    {
        if (!Settings.bAutoClosePairs || (Typed != ')' && Typed != ']' && Typed != '}' && Typed != '"' && Typed != '\''))
        {
            return false;
        }
        for (const FCursor& Cursor : Cursors)
        {
            if (Cursor.HasSelection() || Document.CharAt(Cursor.Head) != Typed)
            {
                return false;
            }
        }
        for (FCursor& Cursor : Cursors)
        {
            Cursor.Head = Document.Next(Cursor.Head);
            Cursor.Anchor = Cursor.Head;
        }
        OnCursorsMoved();
        return true;
    }

    bool FCodeEditor::TryAutoClosePair(char Typed)
    {
        if (!Settings.bAutoClosePairs || Language == nullptr)
        {
            return false;
        }
        const FStringView Pairs = Language->GetAutoClosePairs();
        size_t Index = FStringView::npos;
        for (size_t i = 0; i + 1 < Pairs.size(); i += 2)
        {
            if (Pairs[i] == Typed)
            {
                Index = i;
                break;
            }
        }
        if (Index == FStringView::npos)
        {
            return false;
        }
        const char Closer = Pairs[Index + 1];
        const bool bQuote = Typed == Closer;

        if (HasSelection())
        {
            for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
            {
                FCursor& Cursor = Cursors[i];
                if (!Cursor.HasSelection())
                {
                    continue;
                }
                const FCodeRange Range = Cursor.GetRange();
                const bool bForward = Cursor.Anchor < Cursor.Head;
                InsertRaw(Range.End, FStringView(&Closer, 1));
                InsertRaw(Range.Start, FStringView(&Typed, 1));
                const FCodePosition Start{ Range.Start.Line, Range.Start.Column + 1 };
                const FCodePosition End{ Range.End.Line, Range.End.Column + (Range.End.Line == Range.Start.Line ? 1 : 0) };
                Cursor.Anchor = bForward ? Start : End;
                Cursor.Head = bForward ? End : Start;
            }
            return true;
        }

        for (const FCursor& Cursor : Cursors)
        {
            const char Next = Document.CharAt(Cursor.Head);
            const bool bNextAllows = Next == 0 || Next == '\n' || CodeText::IsSpace(Next) || Next == ')' || Next == ']' || Next == '}' || Next == ';' || Next == ',' || Next == '>' || Next == '"' || Next == '\'';
            if (!bNextAllows)
            {
                return false;
            }
            if (bQuote)
            {
                const char Previous = Cursor.Head.Column > 0 ? Document.GetLine(Cursor.Head.Line)[Cursor.Head.Column - 1] : ' ';
                if (Language->IsWordChar(Previous) || Previous == Typed)
                {
                    return false;
                }
                const ECodeToken Token = Document.GetTokenAt({ Cursor.Head.Line, Cursor.Head.Column - 1 });
                if (Token == ECodeToken::Comment)
                {
                    return false;
                }
            }
        }

        const char Pair[2] = { Typed, Closer };
        for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
        {
            const FCodePosition At = Cursors[i].Head;
            InsertRaw(At, FStringView(Pair, 2));
            Cursors[i].Head = { At.Line, At.Column + 1 };
            Cursors[i].Anchor = Cursors[i].Head;
        }
        return true;
    }

    void FCodeEditor::ApplyAutoInsert(char Typed)
    {
        if (Language == nullptr || Cursors.size() != 1)
        {
            return;
        }
        const FCodePosition Caret = Cursors[0].Head;
        const uint32 State = StateAt(Caret);
        Document.Highlight(*Language, Caret.Line);
        const FString Snippet = Language->GetAutoInsert(Document, Caret, State, Typed);
        if (!Snippet.empty())
        {
            BeginSnippet(Caret, Snippet.View());
        }
    }

    void FCodeEditor::OutdentLineIfTriggered(char Typed)
    {
        if (!Settings.bAutoIndent || Language == nullptr)
        {
            return;
        }
        for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
        {
            const FCodePosition Head = Cursors[i].Head;
            const FStringView Line = Document.GetLineView(Head.Line);
            if (!Language->ShouldOutdentOnType(Line.substr(0, Head.Column), Typed))
            {
                continue;
            }
            const int32 Indent = CodeText::LeadingWhitespace(Line);
            if (Indent == 0)
            {
                continue;
            }
            int32 Remove = 1;
            if (Line[Indent - 1] == ' ')
            {
                const int32 TabSize = Math::Max(1, Settings.TabSize);
                const int32 Visual = CodeText::VisualColumn(Line, Indent, TabSize);
                const int32 Target = ((Visual - 1) / TabSize) * TabSize;
                Remove = 0;
                while (Remove < Indent && Line[Indent - 1 - Remove] == ' ' && Visual - Remove > Target)
                {
                    ++Remove;
                }
            }
            EraseRaw({ { Head.Line, Indent - Remove }, { Head.Line, Indent } });
        }
    }

    void FCodeEditor::NewLine()
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        BeginEdit(EEditKind::Other);
        DeleteSelections();
        for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
        {
            FCodePosition Caret = Cursors[i].Head;
            const FString LineText = Document.GetLine(Caret.Line);
            const FStringView Line = LineText.View();
            const FStringView Before = Line.substr(0, Caret.Column);
            const FStringView After = Line.substr(Caret.Column);

            FString Indent;
            bool bIndentMore = false;
            bool bSplitBlock = false;
            if (Settings.bAutoIndent)
            {
                Indent = FString(Line.substr(0, Math::Min(CodeText::LeadingWhitespace(Line), Caret.Column)));
                const uint32 State = StateAt(Caret);
                bIndentMore = Language != nullptr ? Language->ShouldIndentAfter(Before, State) : (!Before.empty() && (Before.back() == '{' || Before.back() == '('));
                const FStringView Rest = After.substr(CodeText::LeadingWhitespace(After));
                bSplitBlock = bIndentMore && !Rest.empty() && (Rest[0] == '}' || Rest[0] == ')' || Rest[0] == ']' || Rest.starts_with("</"));

                const int32 TrailingSpaces = CodeText::LeadingWhitespace(After);
                if (TrailingSpaces > 0)
                {
                    EraseRaw({ Caret, { Caret.Line, Caret.Column + TrailingSpaces } });
                }
                if (CodeText::IsBlank(Before) && !Before.empty())
                {
                    EraseRaw({ { Caret.Line, 0 }, Caret });
                    Caret.Column = 0;
                }
            }

            FString Text("\n");
            Text += Indent;
            if (bIndentMore)
            {
                Text += IndentUnit();
            }
            const int32 CaretColumn = (int32)Text.size() - 1;
            if (bSplitBlock)
            {
                Text += '\n';
                Text += Indent;
            }
            InsertRaw(Caret, Text.View());
            Cursors[i].Head = { Caret.Line + 1, CaretColumn };
            Cursors[i].Anchor = Cursors[i].Head;
            Cursors[i].PreferredVisual = -1;
        }
        EndEdit();
        CloseCompletion();
    }

    void FCodeEditor::Backspace(bool bWord)
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        BeginEdit(EEditKind::Deleting);
        if (HasSelection())
        {
            DeleteSelections();
            EndEdit();
            return;
        }

        const FStringView Pairs = Language != nullptr ? Language->GetAutoClosePairs() : FStringView("()[]{}\"\"''");
        for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
        {
            const FCodePosition Caret = Cursors[i].Head;
            if (Caret == FCodePosition())
            {
                continue;
            }
            const FStringView Line = Document.GetLineView(Caret.Line);
            FCodePosition Start = Document.Prev(Caret);
            FCodePosition End = Caret;
            if (bWord)
            {
                Start = WordBoundary(Caret, false);
            }
            else if (Caret.Column > 0)
            {
                const char Before = Line[Caret.Column - 1];
                const char After = Caret.Column < (int32)Line.size() ? Line[Caret.Column] : 0;
                bool bPair = false;
                for (size_t p = 0; Settings.bAutoClosePairs && p + 1 < Pairs.size(); p += 2)
                {
                    bPair |= Pairs[p] == Before && Pairs[p + 1] == After;
                }
                if (bPair)
                {
                    End.Column += 1;
                }
                else if (Settings.bInsertSpaces && Before == ' ' && Caret.Column <= CodeText::LeadingWhitespace(Line))
                {
                    const int32 TabSize = Math::Max(1, Settings.TabSize);
                    const int32 Visual = CodeText::VisualColumn(Line, Caret.Column, TabSize);
                    const int32 Target = ((Visual - 1) / TabSize) * TabSize;
                    int32 Column = Caret.Column;
                    while (Column > 0 && Line[Column - 1] == ' ' && CodeText::VisualColumn(Line, Column, TabSize) > Target)
                    {
                        --Column;
                    }
                    Start.Column = Column;
                }
            }
            EraseRaw({ Start, End });
        }
        EndEdit();
    }

    void FCodeEditor::Delete(bool bWord)
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        BeginEdit(EEditKind::Deleting);
        if (HasSelection())
        {
            DeleteSelections();
            EndEdit();
            return;
        }
        for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
        {
            const FCodePosition Caret = Cursors[i].Head;
            EraseRaw({ Caret, bWord ? WordBoundary(Caret, true) : Document.Next(Caret) });
        }
        EndEdit();
    }

    void FCodeEditor::Copy() const
    {
        FString Text;
        if (HasSelection())
        {
            for (const FCursor& Cursor : Cursors)
            {
                if (!Cursor.HasSelection())
                {
                    continue;
                }
                if (!Text.empty())
                {
                    Text += '\n';
                }
                Text += Document.GetRangeText(Cursor.GetRange());
            }
        }
        else
        {
            for (const FCursor& Cursor : Cursors)
            {
                Text += Document.GetLine(Cursor.Head.Line);
                Text += '\n';
            }
        }
        ImGui::SetClipboardText(Text.c_str());
    }

    void FCodeEditor::Cut()
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        Copy();
        if (HasSelection())
        {
            BeginEdit(EEditKind::Other);
            DeleteSelections();
            EndEdit();
        }
        else
        {
            DeleteLines();
        }
    }

    void FCodeEditor::Paste()
    {
        const char* Clipboard = ImGui::GetClipboardText();
        if (Settings.bReadOnly || Clipboard == nullptr || *Clipboard == '\0')
        {
            return;
        }
        FString Text(Clipboard);
        Text.erase(Algo::Remove(Text.begin(), Text.end(), '\r'), Text.end());

        TVector<FStringView> Pieces;
        if (Cursors.size() > 1)
        {
            size_t Start = 0;
            const FStringView View = Text.View();
            for (size_t i = 0; i <= View.size(); ++i)
            {
                if (i == View.size() || View[i] == '\n')
                {
                    Pieces.push_back(View.substr(Start, i - Start));
                    Start = i + 1;
                }
            }
            if (!Pieces.empty() && Pieces.back().empty())
            {
                Pieces.pop_back();
            }
        }

        BeginEdit(EEditKind::Other);
        if (Pieces.size() == Cursors.size())
        {
            DeleteSelections();
            for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
            {
                InsertRaw(Cursors[i].Head, Pieces[i]);
            }
        }
        else
        {
            TypeAtCursors(Text.View());
        }
        EndEdit();
        CloseCompletion();
    }

    void FCodeEditor::Indent()
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        const FString Unit = IndentUnit();
        BeginEdit(EEditKind::Other);
        for (const int32 Line : SelectedLines())
        {
            if (!CodeText::IsBlank(Document.GetLineView(Line)))
            {
                InsertRaw({ Line, 0 }, Unit.View());
            }
        }
        EndEdit();
    }

    void FCodeEditor::InsertTab()
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        const FCodeRange Selection = Cursors[MainCursor].GetRange();
        if (Selection.Start.Line != Selection.End.Line)
        {
            Indent();
            return;
        }
        const int32 TabSize = Math::Max(1, Settings.TabSize);
        BeginEdit(EEditKind::Other);
        DeleteSelections();
        for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
        {
            const FCodePosition Head = Cursors[i].Head;
            const int32 Visual = CodeText::VisualColumn(Document.GetLineView(Head.Line), Head.Column, TabSize);
            const FString Padding = Settings.bInsertSpaces ? FString((size_t)(TabSize - Visual % TabSize), ' ') : FString("\t");
            InsertRaw(Head, Padding.View());
        }
        EndEdit();
    }

    void FCodeEditor::Outdent()
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        const int32 TabSize = Math::Max(1, Settings.TabSize);
        BeginEdit(EEditKind::Other);
        for (const int32 Line : SelectedLines())
        {
            const FStringView Text = Document.GetLineView(Line);
            int32 Remove = 0;
            if (!Text.empty() && Text[0] == '\t')
            {
                Remove = 1;
            }
            else
            {
                while (Remove < TabSize && Remove < (int32)Text.size() && Text[Remove] == ' ')
                {
                    ++Remove;
                }
            }
            if (Remove > 0)
            {
                EraseRaw({ { Line, 0 }, { Line, Remove } });
            }
        }
        EndEdit();
    }

    void FCodeEditor::MoveLines(int32 Direction)
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        const TVector<int32> Lines = SelectedLines();
        const int32 First = Lines.front();
        const int32 Last = Lines.back();
        if ((Direction < 0 && First == 0) || (Direction > 0 && Last >= Document.GetLineCount() - 1))
        {
            return;
        }

        TVector<FCursor> Saved = Cursors;
        BeginEdit(EEditKind::Other);
        const FString Block = Document.GetRangeText({ { First, 0 }, { Last, (int32)Document.GetLine(Last).size() } });
        if (Direction < 0)
        {
            const FString Above = Document.GetLine(First - 1);
            ReplaceRange({ { First - 1, 0 }, { Last, (int32)Document.GetLine(Last).size() } }, (Block + "\n" + Above).View());
        }
        else
        {
            const FString Below = Document.GetLine(Last + 1);
            ReplaceRange({ { First, 0 }, { Last + 1, (int32)Document.GetLine(Last + 1).size() } }, (Below + "\n" + Block).View());
        }
        for (FCursor& Cursor : Saved)
        {
            Cursor.Anchor.Line += Direction < 0 ? -1 : 1;
            Cursor.Head.Line += Direction < 0 ? -1 : 1;
        }
        Cursors = Move(Saved);
        EndEdit();
    }

    void FCodeEditor::DuplicateLines(int32 Direction)
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        const TVector<int32> Lines = SelectedLines();
        const int32 First = Lines.front();
        const int32 Last = Lines.back();
        const FString Block = Document.GetRangeText({ { First, 0 }, { Last, (int32)Document.GetLine(Last).size() } });

        TVector<FCursor> Saved = Cursors;
        BeginEdit(EEditKind::Other);
        InsertRaw({ Last, (int32)Document.GetLine(Last).size() }, ("\n" + Block).View());
        if (Direction > 0)
        {
            const int32 Shift = Last - First + 1;
            for (FCursor& Cursor : Saved)
            {
                Cursor.Anchor.Line += Shift;
                Cursor.Head.Line += Shift;
            }
        }
        Cursors = Move(Saved);
        EndEdit();
    }

    void FCodeEditor::DeleteLines()
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        const TVector<int32> Lines = SelectedLines();
        BeginEdit(EEditKind::Other);
        for (int32 i = (int32)Lines.size() - 1; i >= 0; --i)
        {
            const int32 Line = Lines[i];
            if (Line + 1 < Document.GetLineCount())
            {
                EraseRaw({ { Line, 0 }, { Line + 1, 0 } });
            }
            else if (Line > 0)
            {
                EraseRaw({ { Line - 1, (int32)Document.GetLine(Line - 1).size() }, { Line, (int32)Document.GetLine(Line).size() } });
            }
            else
            {
                EraseRaw({ { 0, 0 }, { 0, (int32)Document.GetLine(0).size() } });
            }
        }
        for (FCursor& Cursor : Cursors)
        {
            Cursor.Anchor = Cursor.Head;
        }
        EndEdit();
    }

    void FCodeEditor::InsertLine(bool bAbove)
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        BeginEdit(EEditKind::Other);
        for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
        {
            const int32 Line = Cursors[i].Head.Line;
            const FString Indent = LineIndent(Line);
            if (bAbove)
            {
                InsertRaw({ Line, 0 }, (Indent + "\n").View());
                Cursors[i].Head = { Line, (int32)Indent.size() };
            }
            else
            {
                const FCodePosition End = InsertRaw({ Line, (int32)Document.GetLine(Line).size() }, ("\n" + Indent).View());
                Cursors[i].Head = End;
            }
            Cursors[i].Anchor = Cursors[i].Head;
        }
        EndEdit();
    }

    void FCodeEditor::ToggleLineComment()
    {
        if (Settings.bReadOnly || Language == nullptr)
        {
            return;
        }
        const TVector<int32> Lines = SelectedLines();
        const FCodeCommentSyntax Syntax = Language->GetCommentSyntax(StateAt({ Lines.front(), 0 }));
        if (Syntax.Line.empty())
        {
            if (!HasSelection())
            {
                const int32 Line = Lines.front();
                const FStringView Text = Document.GetLineView(Line);
                Select({ { Line, CodeText::LeadingWhitespace(Text) }, { Line, (int32)Text.size() } }, false);
            }
            ToggleBlockComment();
            return;
        }

        bool bAllCommented = true;
        int32 MinIndent = INT32_MAX;
        for (const int32 Line : Lines)
        {
            const FStringView Text = Document.GetLineView(Line);
            if (CodeText::IsBlank(Text))
            {
                continue;
            }
            const int32 Indent = CodeText::LeadingWhitespace(Text);
            MinIndent = Math::Min(MinIndent, Indent);
            bAllCommented &= Text.substr(Indent).starts_with(Syntax.Line);
        }
        if (MinIndent == INT32_MAX)
        {
            return;
        }

        BeginEdit(EEditKind::Other);
        for (const int32 Line : Lines)
        {
            const FStringView Text = Document.GetLineView(Line);
            if (CodeText::IsBlank(Text))
            {
                continue;
            }
            if (bAllCommented)
            {
                const int32 Indent = CodeText::LeadingWhitespace(Text);
                int32 Length = (int32)Syntax.Line.size();
                if (Indent + Length < (int32)Text.size() && Text[Indent + Length] == ' ')
                {
                    ++Length;
                }
                EraseRaw({ { Line, Indent }, { Line, Indent + Length } });
            }
            else
            {
                InsertRaw({ Line, MinIndent }, (FString(Syntax.Line) + " ").View());
            }
        }
        EndEdit();
    }

    void FCodeEditor::ToggleBlockComment()
    {
        if (Settings.bReadOnly || Language == nullptr)
        {
            return;
        }
        BeginEdit(EEditKind::Other);
        for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
        {
            FCodeRange Range = Cursors[i].GetRange();
            if (Range.IsEmpty())
            {
                const FStringView Text = Document.GetLineView(Range.Start.Line);
                Range = { { Range.Start.Line, CodeText::LeadingWhitespace(Text) }, { Range.Start.Line, (int32)Text.size() } };
            }
            const FCodeCommentSyntax Syntax = Language->GetCommentSyntax(StateAt(Range.Start));
            if (Syntax.BlockOpen.empty())
            {
                continue;
            }
            const FString Text = Document.GetRangeText(Range);
            const FStringView View = Text.View();
            if (View.starts_with(Syntax.BlockOpen) && View.ends_with(Syntax.BlockClose) && View.size() >= Syntax.BlockOpen.size() + Syntax.BlockClose.size())
            {
                FStringView Inner = View.substr(Syntax.BlockOpen.size(), View.size() - Syntax.BlockOpen.size() - Syntax.BlockClose.size());
                if (Inner.starts_with(" "))
                {
                    Inner = Inner.substr(1);
                }
                if (Inner.ends_with(" "))
                {
                    Inner = Inner.substr(0, Inner.size() - 1);
                }
                const FString Unwrapped(Inner);
                ReplaceRange(Range, Unwrapped.View());
                Cursors[i].Anchor = Range.Start;
                Cursors[i].Head = AdvanceThrough(Range.Start, Unwrapped.View(), Unwrapped.size());
            }
            else
            {
                const FString Wrapped = FString(Syntax.BlockOpen) + " " + Text + " " + FString(Syntax.BlockClose);
                ReplaceRange(Range, Wrapped.View());
                Cursors[i].Anchor = Range.Start;
                Cursors[i].Head = AdvanceThrough(Range.Start, Wrapped.View(), Wrapped.size());
            }
        }
        EndEdit();
    }

    void FCodeEditor::ChangeCase(bool bUpper)
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        TVector<FCursor> Saved = Cursors;
        BeginEdit(EEditKind::Other);
        for (int32 i = (int32)Cursors.size() - 1; i >= 0; --i)
        {
            const FCodeRange Range = Cursors[i].GetRange();
            FString Text = Document.GetRangeText(Range);
            for (char& C : Text)
            {
                C = bUpper ? CodeText::ToUpper(C) : CodeText::ToLower(C);
            }
            ReplaceRange(Range, Text.View());
        }
        Cursors = Move(Saved);
        EndEdit();
    }

    void FCodeEditor::TrimTrailingWhitespace()
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        BeginEdit(EEditKind::Other);
        for (int32 Line = 0; Line < Document.GetLineCount(); ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            int32 End = (int32)Text.size();
            while (End > 0 && CodeText::IsSpace(Text[End - 1]))
            {
                --End;
            }
            if (End < (int32)Text.size())
            {
                EraseRaw({ { Line, End }, { Line, (int32)Text.size() } });
            }
        }
        EndEdit();
    }

    void FCodeEditor::ConvertIndentation(bool bToSpaces)
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        const int32 TabSize = Math::Max(1, Settings.TabSize);
        BeginEdit(EEditKind::Other);
        for (int32 Line = 0; Line < Document.GetLineCount(); ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            const int32 Indent = CodeText::LeadingWhitespace(Text);
            const int32 Visual = CodeText::VisualColumn(Text, Indent, TabSize);
            const FString Converted = bToSpaces
                ? FString((size_t)Visual, ' ')
                : FString((size_t)(Visual / TabSize), '\t') + FString((size_t)(Visual % TabSize), ' ');
            if (Converted.View() != Text.substr(0, Indent))
            {
                ReplaceRange({ { Line, 0 }, { Line, Indent } }, Converted.View());
            }
        }
        EndEdit();
    }

    void FCodeEditor::AddNextOccurrence()
    {
        FCursor& Main = Cursors[MainCursor];
        if (!Main.HasSelection())
        {
            const FCodeRange Word = WordAt(Main.Head);
            if (!Word.IsEmpty())
            {
                Main.Anchor = Word.Start;
                Main.Head = Word.End;
            }
            OnCursorsMoved();
            return;
        }

        const FString Needle = Document.GetRangeText(Main.GetRange());
        if (Needle.empty() || Needle.contains('\n'))
        {
            return;
        }
        FCodePosition From = Cursors.back().GetRange().End;
        for (int32 Pass = 0; Pass < 2; ++Pass)
        {
            for (int32 Line = From.Line; Line < Document.GetLineCount(); ++Line)
            {
                const FStringView Text = Document.GetLineView(Line);
                size_t Found = Text.find(Needle.View(), Line == From.Line ? From.Column : 0);
                while (Found != FStringView::npos)
                {
                    const FCodeRange Range{ { Line, (int32)Found }, { Line, (int32)(Found + Needle.size()) } };
                    const bool bTaken = Algo::AnyOf(Cursors.begin(), Cursors.end(), [&Range](const FCursor& Cursor) { return Cursor.GetRange() == Range; });
                    if (!bTaken)
                    {
                        FCursor& Added = Cursors.emplace_back();
                        Added.Anchor = Range.Start;
                        Added.Head = Range.End;
                        MainCursor = (int32)Cursors.size() - 1;
                        NormalizeCursors();
                        EnsureCursorVisible();
                        OnCursorsMoved();
                        return;
                    }
                    Found = Text.find(Needle.View(), Found + 1);
                }
            }
            From = {};
        }
    }

    void FCodeEditor::SelectAllOccurrences()
    {
        FCursor Main = Cursors[MainCursor];
        if (!Main.HasSelection())
        {
            const FCodeRange Word = WordAt(Main.Head);
            Main.Anchor = Word.Start;
            Main.Head = Word.End;
        }
        const FString Needle = Document.GetRangeText(Main.GetRange());
        if (Needle.empty() || Needle.contains('\n'))
        {
            return;
        }
        Cursors.clear();
        for (int32 Line = 0; Line < Document.GetLineCount(); ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            for (size_t Found = Text.find(Needle.View()); Found != FStringView::npos; Found = Text.find(Needle.View(), Found + Needle.size()))
            {
                FCursor& Added = Cursors.emplace_back();
                Added.Anchor = { Line, (int32)Found };
                Added.Head = { Line, (int32)(Found + Needle.size()) };
                if (Added.Anchor == Main.GetRange().Start)
                {
                    MainCursor = (int32)Cursors.size() - 1;
                }
            }
        }
        if (Cursors.empty())
        {
            Cursors.push_back(Main);
            MainCursor = 0;
        }
        NormalizeCursors();
        OnCursorsMoved();
    }

    void FCodeEditor::AddCursorVertical(int32 Direction)
    {
        const int32 TabSize = Math::Max(1, Settings.TabSize);
        int32 Edge = 0;
        for (int32 i = 1; i < (int32)Cursors.size(); ++i)
        {
            const bool bFurther = Direction < 0 ? Cursors[i].Head.Line < Cursors[Edge].Head.Line : Cursors[i].Head.Line > Cursors[Edge].Head.Line;
            Edge = bFurther ? i : Edge;
        }
        const FCursor& From = Cursors[Edge];
        const int32 Line = From.Head.Line + (Direction < 0 ? -1 : 1);
        if (Line < 0 || Line >= Document.GetLineCount())
        {
            return;
        }
        const int32 Visual = From.PreferredVisual >= 0 ? From.PreferredVisual : CodeText::VisualColumn(Document.GetLineView(From.Head.Line), From.Head.Column, TabSize);
        FCursor Added;
        Added.Head = { Line, CodeText::ColumnAtVisual(Document.GetLineView(Line), Visual, TabSize, false) };
        Added.Anchor = Added.Head;
        Added.PreferredVisual = Visual;
        Cursors.push_back(Added);
        MainCursor = (int32)Cursors.size() - 1;
        NormalizeCursors();
        EnsureCursorVisible();
        OnCursorsMoved();
    }

    void FCodeEditor::JumpToMatchingBracket()
    {
        FCodePosition Open;
        FCodePosition Close;
        if (!FindBracketPair(Cursors[MainCursor].Head, Open, Close))
        {
            return;
        }
        const FCodePosition Head = Cursors[MainCursor].Head;
        SetCursor(Head == Open || Head == FCodePosition{ Open.Line, Open.Column + 1 } ? Close : Open, false);
    }

    bool FCodeEditor::GoToDefinition(FCodePosition Position)
    {
        FCodeLocation Location;
        if (Language == nullptr || !Language->FindDefinition(Document, Document.Clamp(Position), Location))
        {
            return false;
        }
        if (Location.Path.empty() || !DefinitionHandler)
        {
            SetCursor(Location.Position, true);
        }
        else
        {
            DefinitionHandler(Location);
        }
        return true;
    }

    bool FCodeEditor::FindBracketPair(FCodePosition Caret, FCodePosition& OutOpen, FCodePosition& OutClose)
    {
        constexpr int32 MaxScanLines = 400;
        auto IsCode = [this](FCodePosition Position)
        {
            const ECodeToken Token = Document.GetTokenAt(Position);
            return Token != ECodeToken::String && Token != ECodeToken::Comment;
        };
        auto OpenerOf = [](char C) -> char { return C == ')' ? '(' : C == ']' ? '[' : C == '}' ? '{' : 0; };
        auto CloserOf = [](char C) -> char { return C == '(' ? ')' : C == '[' ? ']' : C == '{' ? '}' : 0; };

        if (Language != nullptr)
        {
            Document.Highlight(*Language, Math::Min(Document.GetLineCount() - 1, Caret.Line + MaxScanLines));
        }

        FCodePosition At = Caret;
        char C = Document.CharAt(At);
        if (CloserOf(C) == 0 && OpenerOf(C) == 0 && At.Column > 0)
        {
            At = Document.Prev(At);
            C = Document.CharAt(At);
        }
        if ((CloserOf(C) == 0 && OpenerOf(C) == 0) || !IsCode(At))
        {
            return false;
        }

        const bool bForward = CloserOf(C) != 0;
        const char Target = bForward ? CloserOf(C) : OpenerOf(C);
        int32 Depth = 0;
        FCodePosition Scan = At;
        while (true)
        {
            const FCodePosition Next = bForward ? Document.Next(Scan) : Document.Prev(Scan);
            if (Next == Scan || Math::Abs(Next.Line - At.Line) > MaxScanLines)
            {
                return false;
            }
            Scan = Next;
            const char S = Document.CharAt(Scan);
            if (!IsCode(Scan))
            {
                continue;
            }
            if (S == C)
            {
                ++Depth;
            }
            else if (S == Target)
            {
                if (Depth == 0)
                {
                    OutOpen = bForward ? At : Scan;
                    OutClose = bForward ? Scan : At;
                    return true;
                }
                --Depth;
            }
        }
    }

    void FCodeEditor::BeginSnippet(FCodePosition At, FStringView Snippet)
    {
        const FString BaseIndent = LineIndent(At.Line);
        const FString Unit = IndentUnit();

        struct FParsedStop
        {
            int32  Index = 0;
            size_t Start = 0;
            size_t End = 0;
        };
        TVector<FParsedStop> Parsed;
        FString Text;
        for (size_t i = 0; i < Snippet.size(); ++i)
        {
            const char C = Snippet[i];
            if (C == '$' && i + 1 < Snippet.size() && CodeText::IsDigit(Snippet[i + 1]))
            {
                Parsed.push_back({ Snippet[i + 1] - '0', Text.size(), Text.size() });
                ++i;
                continue;
            }
            if (C == '$' && i + 3 < Snippet.size() && Snippet[i + 1] == '{' && CodeText::IsDigit(Snippet[i + 2]) && Snippet[i + 3] == ':')
            {
                const size_t Close = Snippet.find('}', i + 4);
                if (Close != FStringView::npos)
                {
                    const size_t Start = Text.size();
                    Text += Snippet.substr(i + 4, Close - (i + 4));
                    Parsed.push_back({ Snippet[i + 2] - '0', Start, Text.size() });
                    i = Close;
                    continue;
                }
            }
            if (C == '\n')
            {
                Text += '\n';
                Text += BaseIndent;
                continue;
            }
            if (C == '\t')
            {
                Text += Unit;
                continue;
            }
            Text += C;
        }

        ClearExtraCursors();
        const FCodePosition End = InsertRaw(At, Text.View());

        SnippetStops.clear();
        SnippetCurrent = -1;
        FCodePosition Final = End;
        for (const FParsedStop& Stop : Parsed)
        {
            const FCodeRange Range{ AdvanceThrough(At, Text.View(), Stop.Start), AdvanceThrough(At, Text.View(), Stop.End) };
            if (Stop.Index == 0)
            {
                Final = Range.Start;
                continue;
            }
            SnippetStops.push_back({ Stop.Index, Range });
        }
        Algo::StableSort(SnippetStops.begin(), SnippetStops.end(), [](const FSnippetStop& A, const FSnippetStop& B) { return A.Index < B.Index; });

        if (SnippetStops.empty())
        {
            Cursors[0].Anchor = Final;
            Cursors[0].Head = Final;
            return;
        }
        SnippetStops.push_back({ 0, { Final, Final } });
        SnippetCurrent = 0;
        Cursors[0].Anchor = SnippetStops[0].Range.Start;
        Cursors[0].Head = SnippetStops[0].Range.End;
    }

    void FCodeEditor::InsertSnippet(FStringView Snippet)
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        BeginEdit(EEditKind::Other);
        DeleteSelections();
        BeginSnippet(Cursors[MainCursor].Head, Snippet);
        EndEdit();
        Focus();
    }

    bool FCodeEditor::AdvanceSnippet(int32 Direction)
    {
        if (SnippetCurrent < 0 || SnippetStops.empty())
        {
            return false;
        }
        SnippetCurrent = Math::Clamp(SnippetCurrent + Direction, 0, (int32)SnippetStops.size() - 1);
        const FSnippetStop Stop = SnippetStops[SnippetCurrent];
        ClearExtraCursors();
        Cursors[0].Anchor = Stop.Range.Start;
        Cursors[0].Head = Stop.Range.End;
        if (Stop.Index == 0)
        {
            SnippetStops.clear();
            SnippetCurrent = -1;
        }
        EnsureCursorVisible();
        CursorBlinkStart = Now();
        return true;
    }

    FCodeCompletionRequest FCodeEditor::MakeCompletionRequest(char TriggerChar, bool bExplicit)
    {
        FCodeCompletionRequest Request;
        Request.Document = &Document;
        Request.Caret = Cursors[MainCursor].Head;
        Request.WordStart = WordAt(Request.Caret).Start;
        if (Request.WordStart.Column > Request.Caret.Column)
        {
            Request.WordStart = Request.Caret;
        }
        const FStringView Line = Document.GetLineView(Request.Caret.Line);
        Request.Word = Line.substr(Request.WordStart.Column, Request.Caret.Column - Request.WordStart.Column);
        Request.LinePrefix = Line.substr(0, Request.Caret.Column);
        Request.CaretState = StateAt(Request.Caret);
        if (Language != nullptr)
        {
            Document.Highlight(*Language, Request.Caret.Line);
        }
        Request.TriggerChar = TriggerChar;
        Request.bExplicit = bExplicit;
        return Request;
    }

    void FCodeEditor::OpenCompletion(char TriggerChar, bool bExplicit)
    {
        if (Cursors.size() != 1 || (Language == nullptr && !CompletionSource.Gather))
        {
            CloseCompletion();
            return;
        }
        const FCodeCompletionRequest Request = MakeCompletionRequest(TriggerChar, bExplicit);
        Completion.Items.clear();
        if (Language != nullptr)
        {
            Language->GetCompletions(Request, Completion.Items);
        }
        if (CompletionSource.Gather)
        {
            CompletionSource.Gather(Request, Completion.Items);
        }
        if (Completion.Items.empty())
        {
            CloseCompletion();
            return;
        }
        Completion.bOpen = true;
        Completion.WordStart = Request.WordStart;
        Completion.Selected = 0;
        Completion.FirstRow = 0;
        Completion.FilteredVersion = Document.GetVersion();
        FilterCompletion();
        if (Completion.Visible.empty())
        {
            CloseCompletion();
        }
    }

    void FCodeEditor::FilterCompletion()
    {
        const FCodePosition Caret = Cursors[MainCursor].Head;
        const FString Word(Document.GetLineView(Caret.Line).substr(Completion.WordStart.Column, Caret.Column - Completion.WordStart.Column));
        Completion.FilteredCaret = Caret;
        Completion.Visible.clear();
        for (int32 i = 0; i < (int32)Completion.Items.size(); ++i)
        {
            const FCodeCompletionItem& Item = Completion.Items[i];
            FCompletionEntry Entry;
            Entry.Item = i;
            if (!Word.empty())
            {
                int32 Score = 0;
                if (!FuzzyMatch(Word.c_str(), Item.Label.c_str(), Score, Entry.Matches))
                {
                    continue;
                }
                Entry.Score = Score + Item.SortBoost + (Item.Label.starts_with(Word.View()) ? 25 : 0);
            }
            else
            {
                Entry.Score = Item.SortBoost;
            }
            Completion.Visible.push_back(Move(Entry));
        }

        const TVector<FCodeCompletionItem>& Items = Completion.Items;
        Algo::StableSort(Completion.Visible.begin(), Completion.Visible.end(), [&Items](const FCompletionEntry& A, const FCompletionEntry& B)
        {
            if (A.Score != B.Score)
            {
                return A.Score > B.Score;
            }
            const FString& LabelA = Items[A.Item].Label;
            const FString& LabelB = Items[B.Item].Label;
            if (LabelA.size() != LabelB.size())
            {
                return LabelA.size() < LabelB.size();
            }
            return LabelA < LabelB;
        });

        // A word already typed out in full leaves nothing to suggest.
        if (Completion.Visible.size() == 1 && Items[Completion.Visible[0].Item].Label == Word && Items[Completion.Visible[0].Item].InsertText.empty())
        {
            Completion.Visible.clear();
        }
        Completion.Selected = Math::Clamp(Completion.Selected, 0, Math::Max(0, (int32)Completion.Visible.size() - 1));
        Completion.bScrollToSelected = true;
    }

    void FCodeEditor::RefreshCompletion()
    {
        if (!Completion.bOpen)
        {
            return;
        }
        const FCodePosition Caret = Cursors[MainCursor].Head;
        if (Cursors.size() != 1 || Cursors[MainCursor].HasSelection() || Caret.Line != Completion.WordStart.Line || Caret < Completion.WordStart)
        {
            CloseCompletion();
            return;
        }
        const FStringView Typed = Document.GetLineView(Caret.Line).substr(Completion.WordStart.Column, Caret.Column - Completion.WordStart.Column);
        for (char C : Typed)
        {
            if (Language != nullptr ? !Language->IsWordChar(C) : !(CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '_'))
            {
                CloseCompletion();
                return;
            }
        }
        if (Completion.FilteredVersion != Document.GetVersion())
        {
            const int32 Selected = Completion.Selected;
            OpenCompletion(0, true);
            Completion.Selected = Selected;
            if (Completion.bOpen)
            {
                FilterCompletion();
            }
            return;
        }
        if (Completion.FilteredCaret != Caret)
        {
            FilterCompletion();
        }
    }

    void FCodeEditor::AcceptCompletion()
    {
        if (!Completion.bOpen || Completion.Visible.empty())
        {
            CloseCompletion();
            return;
        }
        const FCodeCompletionItem Item = Completion.Items[Completion.Visible[Completion.Selected].Item];
        const FCodeRange Replace{ Completion.WordStart, Cursors[MainCursor].Head };
        CloseCompletion();

        BeginEdit(EEditKind::Other);
        EraseRaw(Replace);
        BeginSnippet(Replace.Start, Item.InsertText.empty() ? Item.Label.View() : Item.InsertText.View());
        EndEdit();

        if (Item.bRetrigger)
        {
            OpenCompletion(0, false);
        }
        if (Language != nullptr)
        {
            RefreshSignature();
        }
    }

    void FCodeEditor::TriggerCompletion()
    {
        OpenCompletion(0, true);
    }

    void FCodeEditor::TriggerSignatureHelp()
    {
        bSignatureOpen = true;
        RefreshSignature();
    }

    void FCodeEditor::RefreshSignature()
    {
        if (Language == nullptr || Cursors.size() != 1)
        {
            bSignatureOpen = false;
            return;
        }
        const FCodePosition Caret = Cursors[MainCursor].Head;
        Document.Highlight(*Language, Caret.Line);
        Signature = {};
        bSignatureOpen = Language->GetSignature(Document, Caret, Signature);
        SignatureCaret = Caret;
    }

    void FCodeEditor::MaybeTriggerAfterTyping(char Typed)
    {
        if (Cursors.size() != 1)
        {
            CloseCompletion();
            return;
        }
        if (Typed == '(' || Typed == ',' || Typed == ' ')
        {
            TriggerSignatureHelp();
        }
        else if (bSignatureOpen)
        {
            RefreshSignature();
        }

        const bool bWordChar = Language != nullptr ? Language->IsWordChar(Typed) : (CodeText::IsAlpha(Typed) || CodeText::IsDigit(Typed) || Typed == '_');
        if (Completion.bOpen && bWordChar)
        {
            return;
        }

        const FCodeCompletionRequest Request = MakeCompletionRequest(Typed, false);
        const bool bLanguageTrigger = Language != nullptr && Language->IsCompletionTrigger(Typed, Request.LinePrefix, Request.CaretState);
        const bool bSourceTrigger = CompletionSource.IsTrigger && CompletionSource.IsTrigger(Typed, Request);
        if (bLanguageTrigger || bSourceTrigger)
        {
            OpenCompletion(Typed, false);
        }
        else if (bWordChar && Settings.bSuggestWhileTyping)
        {
            OpenCompletion(0, false);
        }
        else
        {
            CloseCompletion();
        }
    }

    void FCodeEditor::OpenFind(bool bReplace)
    {
        Find.bOpen = true;
        Find.bReplace = bReplace;
        Find.bFocusQuery = true;
        const FCodeRange Selection = Cursors[MainCursor].GetRange();
        if (!Selection.IsEmpty() && Selection.Start.Line == Selection.End.Line)
        {
            const FString Text = Document.GetRangeText(Selection);
            const size_t Count = Math::Min(Text.size(), sizeof(Find.Query) - 1);
            memcpy(Find.Query, Text.data(), Count);
            Find.Query[Count] = '\0';
        }
        Find.MatchesVersion = ~0ull;
        CloseCompletion();
    }

    void FCodeEditor::UpdateFindMatches()
    {
        const FStringView Query(Find.Query);
        if (!Find.bOpen || Query.empty())
        {
            Find.Matches.clear();
            Find.MatchesQuery.clear();
            return;
        }
        if (Find.MatchesVersion == Document.GetVersion() && Find.MatchesQuery.View() == Query && Find.bMatchesCase == Find.bMatchCase && Find.bMatchesWord == Find.bWholeWord)
        {
            return;
        }
        Find.MatchesVersion = Document.GetVersion();
        Find.MatchesQuery = Query;
        Find.bMatchesCase = Find.bMatchCase;
        Find.bMatchesWord = Find.bWholeWord;
        Find.Matches.clear();

        auto IsWord = [this](char C)
        {
            return Language != nullptr ? Language->IsWordChar(C) : (CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '_');
        };
        for (int32 Line = 0; Line < Document.GetLineCount() && (int32)Find.Matches.size() < MaxFindMatches; ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            int32 From = 0;
            while (true)
            {
                const int32 Found = Find.bMatchCase ? (int32)Text.find(Query, From) : CodeText::FindIgnoreCase(Text, Query, From);
                if (Found < 0 || Found == (int32)FStringView::npos)
                {
                    break;
                }
                const int32 End = Found + (int32)Query.size();
                const bool bWhole = !Find.bWholeWord || ((Found == 0 || !IsWord(Text[Found - 1])) && (End >= (int32)Text.size() || !IsWord(Text[End])));
                if (bWhole)
                {
                    Find.Matches.push_back({ { Line, Found }, { Line, End } });
                }
                From = Found + 1;
            }
        }
    }

    bool FCodeEditor::FindNext(bool bBackwards)
    {
        UpdateFindMatches();
        if (Find.Matches.empty())
        {
            return false;
        }
        const FCodeRange Selection = Cursors[MainCursor].GetRange();
        const FCodeRange* Target = nullptr;
        if (bBackwards)
        {
            for (int32 i = (int32)Find.Matches.size() - 1; i >= 0 && Target == nullptr; --i)
            {
                Target = Find.Matches[i].Start < Selection.Start ? &Find.Matches[i] : nullptr;
            }
            Target = Target != nullptr ? Target : &Find.Matches.back();
        }
        else
        {
            for (const FCodeRange& Match : Find.Matches)
            {
                if (Selection.End < Match.End || (Selection.IsEmpty() && Selection.Start <= Match.Start))
                {
                    if (!(Match == Selection))
                    {
                        Target = &Match;
                        break;
                    }
                }
            }
            Target = Target != nullptr ? Target : &Find.Matches.front();
        }
        Select(*Target, true);
        return true;
    }

    int32 FCodeEditor::ReplaceAll()
    {
        UpdateFindMatches();
        if (Settings.bReadOnly || Find.Matches.empty())
        {
            return 0;
        }
        const TVector<FCodeRange> Matches = Find.Matches;
        BeginEdit(EEditKind::Other);
        for (int32 i = (int32)Matches.size() - 1; i >= 0; --i)
        {
            ReplaceRange(Matches[i], FStringView(Find.Replacement));
        }
        EndEdit();
        return (int32)Matches.size();
    }

    void FCodeEditor::UpdateDiagnostics()
    {
        if (Language == nullptr || DiagnosticsVersion == Document.GetVersion() || Now() - LastEditTime < DiagnosticsDelay || Now() < DiagnosticsNotBefore)
        {
            return;
        }
        constexpr int32 MaxDiagnostics = 200;
        constexpr double RetrySeconds = 0.25;
        Document.Highlight(*Language, Document.GetLineCount() - 1);
        TVector<FCodeDiagnostic> Found;
        if (!Language->GetDiagnostics(Document, Found))
        {
            DiagnosticsNotBefore = Now() + RetrySeconds;
            return;
        }
        DiagnosticsVersion = Document.GetVersion();
        LanguageDiagnostics = Move(Found);
        if ((int32)LanguageDiagnostics.size() > MaxDiagnostics)
        {
            LanguageDiagnostics.resize(MaxDiagnostics);
        }
    }
}
