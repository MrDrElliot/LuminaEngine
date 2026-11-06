#include "CodeEditor.h"

#include <chrono>
#include "CodeEditorThemes.h"
#include "Config/Config.h"
#include "Core/Delegates/CoreDelegates.h"
#include "FileSystem/FileSystem.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Settings/EditorSettings.h"
#include "UI/Tools/NodeGraph/GraphActionMenu.h"

namespace Lumina
{
    namespace CodeEditorInternal
    {
        constexpr double TypingMergeSeconds = 1.0;
        constexpr int32  MaxUndoRecords = 1000;
        constexpr int32  MaxFindMatches = 10000;
        constexpr double DiagnosticsDelay = 0.3;
        constexpr double ThemeCheckSeconds = 1.0;

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
            for (int32 i = 0; static_cast<size_t>(i) < Offset && i < Text.Len(); ++i)
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
            if (A.Num() != B.Num())
            {
                return false;
            }
            for (int32 i = 0; i < A.Num(); ++i)
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
        Cursors.Emplace();
        ApplyPreferences();
        PreferencesSavedHandle = FCoreDelegates::OnSettingsSaved.AddLambda([this](CClass* Class)
        {
            if (Class == CCodeEditorSettings::StaticClass())
            {
                ApplyPreferences();
            }
        });
    }

    FCodeEditor::~FCodeEditor()
    {
        FCoreDelegates::OnSettingsSaved.Remove(PreferencesSavedHandle);
    }

    void FCodeEditor::ApplyPreferences()
    {
        const CCodeEditorSettings* Preferences = GetDefault<CCodeEditorSettings>();
        Settings.FontScale             = Preferences->FontScale;
        Settings.LineSpacing           = Preferences->LineSpacing;
        Settings.TabSize               = Math::Clamp(Preferences->TabSize, 1, 8);
        Settings.bInsertSpaces         = Preferences->bInsertSpaces;
        Settings.bShowLineNumbers      = Preferences->bShowLineNumbers;
        Settings.bShowWhitespace       = Preferences->bShowWhitespace;
        Settings.bShowMiniMap          = Preferences->bShowMiniMap;
        Settings.bShowIndentGuides     = Preferences->bShowIndentGuides;
        Settings.bAutoIndent           = Preferences->bAutoIndent;
        Settings.bMatchBrackets        = Preferences->bMatchBrackets;
        Settings.bAutoClosePairs       = Preferences->bAutoClosePairs;
        Settings.bSuggestWhileTyping   = Preferences->bSuggestWhileTyping;
        Settings.bHighlightOccurrences = Preferences->bHighlightOccurrences;
        ThemeName = Preferences->Theme.Name;
        ApplyTheme();
    }

    void FCodeEditor::ApplyTheme()
    {
        Palette = FCodeEditorPalette::Dark();
        const FString VirtualPath = CodeEditorThemes::PathOf(ThemeName.View());
        ThemeFile = CodeEditorThemes::Load(ThemeName.View(), Palette) ? FString(*VFS::ResolvePath(VirtualPath.View())) : FString();
        ThemeWriteTime = ThemeFile.IsEmpty() ? 0 : Filesystem::LastWriteTime(ThemeFile.View());
    }

    void FCodeEditor::ReloadEditedTheme()
    {
        const double Time = Now();
        if (ThemeFile.IsEmpty() || Time < NextThemeCheck)
        {
            return;
        }
        NextThemeCheck = Time + ThemeCheckSeconds;
        if (Filesystem::LastWriteTime(ThemeFile.View()) != ThemeWriteTime)
        {
            ApplyTheme();
        }
    }

    void FCodeEditor::SetText(FStringView Text)
    {
        Document.SetText(Text);
        ResetEditState();
    }

    void FCodeEditor::ResetEditState()
    {
        Cursors.Reset();
        Cursors.Emplace();
        MainCursor = 0;
        UndoStack.Reset();
        RedoStack.Reset();
        PendingEdit = {};
        SnippetStops.Reset();
        SnippetCurrent = -1;
        Completion.bOpen = false;
        bSignatureOpen = false;
        LanguageDiagnostics.Reset();
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
            FEditOp& Op = PendingEdit.Ops.EmplaceGetRef();
            Op.Start = At;
            Op.End = End;
            Op.Text = Text.Find('\r') == Constants::kIndexNone ? FString(Text) : Document.GetRangeText({ At, End });
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
        ShiftFoldsForInsert(At, End);
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
            FEditOp& Op = PendingEdit.Ops.EmplaceGetRef();
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
        ShiftFoldsForErase(Range);
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
        if (--EditDepth > 0 || PendingEdit.Ops.IsEmpty())
        {
            return;
        }

        NormalizeCursors();
        PendingEdit.After = Cursors;
        PendingEdit.Time = Now();
        RedoStack.Reset();

        bool bMerged = false;
        if (!UndoStack.IsEmpty() && PendingEdit.Kind != EEditKind::Other)
        {
            FUndoRecord& Top = UndoStack.Last();
            if (Top.Kind == PendingEdit.Kind && PendingEdit.Time - Top.Time < TypingMergeSeconds && SameCursors(Top.After, PendingEdit.Before))
            {
                Top.Ops.Append(std::make_move_iterator(PendingEdit.Ops.begin()), std::make_move_iterator(PendingEdit.Ops.end()));
                Top.After = PendingEdit.After;
                Top.Time = PendingEdit.Time;
                bMerged = true;
            }
        }
        if (!bMerged)
        {
            UndoStack.Add(Move(PendingEdit));
            if (UndoStack.Num() > MaxUndoRecords)
            {
                UndoStack.RemoveAt(0);
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
            for (int32 i = Record.Ops.Num() - 1; i >= 0; --i)
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
        SnippetStops.Reset();
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
        FUndoRecord Record = Move(UndoStack.Last());
        UndoStack.Pop();
        ApplyUndoRecord(Record, true);
        RedoStack.Add(Move(Record));
    }

    void FCodeEditor::Redo()
    {
        if (!CanRedo())
        {
            return;
        }
        FUndoRecord Record = Move(RedoStack.Last());
        RedoStack.Pop();
        ApplyUndoRecord(Record, false);
        UndoStack.Add(Move(Record));
    }

    void FCodeEditor::NormalizeCursors()
    {
        for (FCursor& Cursor : Cursors)
        {
            Cursor.Anchor = Document.Clamp(Cursor.Anchor);
            Cursor.Head = Document.Clamp(Cursor.Head);
        }
        MainCursor = Math::Clamp(MainCursor, 0, Cursors.Num() - 1);
        if (Cursors.Num() == 1)
        {
            return;
        }

        const FCodePosition MainHead = Cursors[MainCursor].Head;
        Algo::Sort(Cursors.begin(), Cursors.end(), [](const FCursor& A, const FCursor& B)
        {
            return A.GetRange().Start < B.GetRange().Start;
        });

        TVector<FCursor> Merged;
        Merged.Reserve(Cursors.Num());
        for (const FCursor& Cursor : Cursors)
        {
            if (!Merged.IsEmpty())
            {
                FCursor& Last = Merged.Last();
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
            Merged.Add(Cursor);
        }
        Cursors = Move(Merged);

        MainCursor = Cursors.Num() - 1;
        for (int32 i = 0; i < Cursors.Num(); ++i)
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
        if (Cursors.Num() > 1)
        {
            const FCursor Main = Cursors[MainCursor];
            Cursors.Reset();
            Cursors.Add(Main);
        }
        MainCursor = 0;
    }

    void FCodeEditor::OnCursorsMoved()
    {
        CursorBlinkStart = Now();
        for (const FCursor& Cursor : Cursors)
        {
            RevealLine(Cursor.Head.Line);
        }
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
                SnippetStops.Reset();
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
        while (End < Text.Len() && IsWord(Text[End]))
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
            if (Column >= Text.Len())
            {
                return Document.Next(Position);
            }
            while (Column < Text.Len() && CodeText::IsSpace(Text[Column]))
            {
                ++Column;
            }
            if (Column < Text.Len())
            {
                const int32 Class = CharClass(Text[Column], IsWord(Text[Column]));
                while (Column < Text.Len() && CharClass(Text[Column], IsWord(Text[Column])) == Class)
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
                return { Position.Line, Document.GetLine(Position.Line).Len() };
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
        // Moves by visible rows, so a folded block is stepped over as the one line it shows as.
        const int32 TargetRow = RowOf(Position.Line) + Delta;
        if (TargetRow < 0)
        {
            return {};
        }
        if (TargetRow >= LineOfRow.Num() && !LineOfRow.IsEmpty())
        {
            return Document.GetEnd();
        }
        const int32 Target = LineOfRow.IsEmpty() ? Math::Clamp(Position.Line + Delta, 0, Document.GetLineCount() - 1) : LineAtRow(TargetRow);
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
                Lines.Add(Line);
            }
        }
        Algo::Sort(Lines.begin(), Lines.end());
        Lines.SetNum(static_cast<int32>(Algo::Unique(Lines.begin(), Lines.end()) - Lines.begin()));
        return Lines;
    }

    FString FCodeEditor::IndentUnit() const
    {
        return Settings.bInsertSpaces ? FString((size_t)Math::Max(1, Settings.TabSize), ' ') : FString("\t");
    }

    FString FCodeEditor::LineIndent(int32 Line) const
    {
        const FStringView Text = Document.GetLineView(Line);
        return FString(Text.Mid(0, CodeText::LeadingWhitespace(Text)));
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
        Scratch.SetNum(Position.Column);
        return Language->Tokenize(Document.GetLineView(Position.Line).Mid(0, Position.Column), Start, Scratch.GetData());
    }

    void FCodeEditor::DeleteSelections()
    {
        for (int32 i = Cursors.Num() - 1; i >= 0; --i)
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
        for (int32 i = Cursors.Num() - 1; i >= 0; --i)
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
        int32 Index = Constants::kIndexNone;
        for (int32 i = 0; i + 1 < Pairs.Len(); i += 2)
        {
            if (Pairs[i] == Typed)
            {
                Index = i;
                break;
            }
        }
        if (Index == Constants::kIndexNone)
        {
            return false;
        }
        const char Closer = Pairs[Index + 1];
        const bool bQuote = Typed == Closer;

        if (HasSelection())
        {
            for (int32 i = Cursors.Num() - 1; i >= 0; --i)
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
        for (int32 i = Cursors.Num() - 1; i >= 0; --i)
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
        if (Language == nullptr || Cursors.Num() != 1)
        {
            return;
        }
        const FCodePosition Caret = Cursors[0].Head;
        const uint32 State = StateAt(Caret);
        Document.Highlight(*Language, Caret.Line);
        const FString Snippet = Language->GetAutoInsert(Document, Caret, State, Typed);
        if (!Snippet.IsEmpty())
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
        for (int32 i = Cursors.Num() - 1; i >= 0; --i)
        {
            const FCodePosition Head = Cursors[i].Head;
            const FStringView Line = Document.GetLineView(Head.Line);
            if (!Language->ShouldOutdentOnType(Line.Mid(0, Head.Column), Typed))
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
        for (int32 i = Cursors.Num() - 1; i >= 0; --i)
        {
            FCodePosition Caret = Cursors[i].Head;
            const FString LineText = Document.GetLine(Caret.Line);
            const FStringView Line = LineText.View();
            const FStringView Before = Line.Mid(0, Caret.Column);
            const FStringView After = Line.Mid(Caret.Column);

            FString Indent;
            bool bIndentMore = false;
            bool bSplitBlock = false;
            if (Settings.bAutoIndent)
            {
                Indent = FString(Line.Mid(0, Math::Min(CodeText::LeadingWhitespace(Line), Caret.Column)));
                const uint32 State = StateAt(Caret);
                bIndentMore = Language != nullptr ? Language->ShouldIndentAfter(Before, State) : (!Before.IsEmpty() && (Before.Last() == '{' || Before.Last() == '('));
                const FStringView Rest = After.Mid(CodeText::LeadingWhitespace(After));
                bSplitBlock = bIndentMore && !Rest.IsEmpty() && (Rest[0] == '}' || Rest[0] == ')' || Rest[0] == ']' || Rest.StartsWith("</"));

                const int32 TrailingSpaces = CodeText::LeadingWhitespace(After);
                if (TrailingSpaces > 0)
                {
                    EraseRaw({ Caret, { Caret.Line, Caret.Column + TrailingSpaces } });
                }
                if (CodeText::IsBlank(Before) && !Before.IsEmpty())
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
            const int32 CaretColumn = Text.Len() - 1;
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
        for (int32 i = Cursors.Num() - 1; i >= 0; --i)
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
                const char After = Caret.Column < Line.Len() ? Line[Caret.Column] : 0;
                bool bPair = false;
                for (int32 p = 0; Settings.bAutoClosePairs && p + 1 < Pairs.Len(); p += 2)
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
        for (int32 i = Cursors.Num() - 1; i >= 0; --i)
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
                if (!Text.IsEmpty())
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
        ImGui::SetClipboardText(*Text);
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
        Text.ReplaceInline("\r", "");

        TVector<FStringView> Pieces;
        if (Cursors.Num() > 1)
        {
            size_t Start = 0;
            const FStringView View = Text.View();
            for (int32 i = 0; i <= View.Len(); ++i)
            {
                if (i == View.Len() || View[i] == '\n')
                {
                    Pieces.Add(View.Mid(IntCastChecked<int32>(Start), IntCastChecked<int32>(i - Start)));
                    Start = i + 1;
                }
            }
            if (!Pieces.IsEmpty() && Pieces.Last().IsEmpty())
            {
                Pieces.Pop();
            }
        }

        BeginEdit(EEditKind::Other);
        if (Pieces.Num() == Cursors.Num())
        {
            DeleteSelections();
            for (int32 i = Cursors.Num() - 1; i >= 0; --i)
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
        for (int32 i = Cursors.Num() - 1; i >= 0; --i)
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
            if (!Text.IsEmpty() && Text[0] == '\t')
            {
                Remove = 1;
            }
            else
            {
                while (Remove < TabSize && Remove < Text.Len() && Text[Remove] == ' ')
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
        const int32 First = Lines[0];
        const int32 Last = Lines.Last();
        if ((Direction < 0 && First == 0) || (Direction > 0 && Last >= Document.GetLineCount() - 1))
        {
            return;
        }

        TVector<FCursor> Saved = Cursors;
        BeginEdit(EEditKind::Other);
        const FString Block = Document.GetRangeText({ { First, 0 }, { Last, Document.GetLine(Last).Len() } });
        if (Direction < 0)
        {
            const FString Above = Document.GetLine(First - 1);
            ReplaceRange({ { First - 1, 0 }, { Last, Document.GetLine(Last).Len() } }, (Block + "\n" + Above).View());
        }
        else
        {
            const FString Below = Document.GetLine(Last + 1);
            ReplaceRange({ { First, 0 }, { Last + 1, Document.GetLine(Last + 1).Len() } }, (Below + "\n" + Block).View());
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
        const int32 First = Lines[0];
        const int32 Last = Lines.Last();
        const FString Block = Document.GetRangeText({ { First, 0 }, { Last, Document.GetLine(Last).Len() } });

        TVector<FCursor> Saved = Cursors;
        BeginEdit(EEditKind::Other);
        InsertRaw({ Last, Document.GetLine(Last).Len() }, ("\n" + Block).View());
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
        for (int32 i = Lines.Num() - 1; i >= 0; --i)
        {
            const int32 Line = Lines[i];
            if (Line + 1 < Document.GetLineCount())
            {
                EraseRaw({ { Line, 0 }, { Line + 1, 0 } });
            }
            else if (Line > 0)
            {
                EraseRaw({ { Line - 1, Document.GetLine(Line - 1).Len() }, { Line, Document.GetLine(Line).Len() } });
            }
            else
            {
                EraseRaw({ { 0, 0 }, { 0, Document.GetLine(0).Len() } });
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
        for (int32 i = Cursors.Num() - 1; i >= 0; --i)
        {
            const int32 Line = Cursors[i].Head.Line;
            const FString Indent = LineIndent(Line);
            if (bAbove)
            {
                InsertRaw({ Line, 0 }, (Indent + "\n").View());
                Cursors[i].Head = { Line, Indent.Len() };
            }
            else
            {
                const FCodePosition End = InsertRaw({ Line, Document.GetLine(Line).Len() }, ("\n" + Indent).View());
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
        const FCodeCommentSyntax Syntax = Language->GetCommentSyntax(StateAt({ Lines[0], 0 }));
        if (Syntax.Line.IsEmpty())
        {
            if (!HasSelection())
            {
                const int32 Line = Lines[0];
                const FStringView Text = Document.GetLineView(Line);
                Select({ { Line, CodeText::LeadingWhitespace(Text) }, { Line, Text.Len() } }, false);
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
            bAllCommented &= Text.Mid(Indent).StartsWith(Syntax.Line);
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
                int32 Length = Syntax.Line.Len();
                if (Indent + Length < Text.Len() && Text[Indent + Length] == ' ')
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
        for (int32 i = Cursors.Num() - 1; i >= 0; --i)
        {
            FCodeRange Range = Cursors[i].GetRange();
            if (Range.IsEmpty())
            {
                const FStringView Text = Document.GetLineView(Range.Start.Line);
                Range = { { Range.Start.Line, CodeText::LeadingWhitespace(Text) }, { Range.Start.Line, Text.Len() } };
            }
            const FCodeCommentSyntax Syntax = Language->GetCommentSyntax(StateAt(Range.Start));
            if (Syntax.BlockOpen.IsEmpty())
            {
                continue;
            }
            const FString Text = Document.GetRangeText(Range);
            const FStringView View = Text.View();
            if (View.StartsWith(Syntax.BlockOpen) && View.EndsWith(Syntax.BlockClose) && View.Len() >= Syntax.BlockOpen.Len() + Syntax.BlockClose.Len())
            {
                FStringView Inner = View.Mid(Syntax.BlockOpen.Len(), View.Len() - Syntax.BlockOpen.Len() - Syntax.BlockClose.Len());
                if (Inner.StartsWith(" "))
                {
                    Inner = Inner.Mid(1);
                }
                if (Inner.EndsWith(" "))
                {
                    Inner = Inner.Mid(0, Inner.Len() - 1);
                }
                const FString Unwrapped(Inner);
                ReplaceRange(Range, Unwrapped.View());
                Cursors[i].Anchor = Range.Start;
                Cursors[i].Head = AdvanceThrough(Range.Start, Unwrapped.View(), Unwrapped.Len());
            }
            else
            {
                const FString Wrapped = FString(Syntax.BlockOpen) + " " + Text + " " + FString(Syntax.BlockClose);
                ReplaceRange(Range, Wrapped.View());
                Cursors[i].Anchor = Range.Start;
                Cursors[i].Head = AdvanceThrough(Range.Start, Wrapped.View(), Wrapped.Len());
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
        for (int32 i = Cursors.Num() - 1; i >= 0; --i)
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
            int32 End = Text.Len();
            while (End > 0 && CodeText::IsSpace(Text[End - 1]))
            {
                --End;
            }
            if (End < Text.Len())
            {
                EraseRaw({ { Line, End }, { Line, Text.Len() } });
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
            if (Converted.View() != Text.Mid(0, Indent))
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
        if (Needle.IsEmpty() || Needle.Contains('\n'))
        {
            return;
        }
        FCodePosition From = Cursors.Last().GetRange().End;
        for (int32 Pass = 0; Pass < 2; ++Pass)
        {
            for (int32 Line = From.Line; Line < Document.GetLineCount(); ++Line)
            {
                const FStringView Text = Document.GetLineView(Line);
                int32 Found = Text.Find(Needle.View(), Line == From.Line ? From.Column : 0);
                while (Found != Constants::kIndexNone)
                {
                    const FCodeRange Range{ { Line, (int32)Found }, { Line, Found + Needle.Len() } };
                    const bool bTaken = Algo::AnyOf(Cursors.begin(), Cursors.end(), [&Range](const FCursor& Cursor) { return Cursor.GetRange() == Range; });
                    if (!bTaken)
                    {
                        FCursor& Added = Cursors.EmplaceGetRef();
                        Added.Anchor = Range.Start;
                        Added.Head = Range.End;
                        MainCursor = Cursors.Num() - 1;
                        NormalizeCursors();
                        EnsureCursorVisible();
                        OnCursorsMoved();
                        return;
                    }
                    Found = Text.Find(Needle.View(), Found + 1);
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
        if (Needle.IsEmpty() || Needle.Contains('\n'))
        {
            return;
        }
        Cursors.Reset();
        for (int32 Line = 0; Line < Document.GetLineCount(); ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            for (int32 Found = Text.Find(Needle.View()); Found != Constants::kIndexNone; Found = Text.Find(Needle.View(), Found + Needle.Len()))
            {
                FCursor& Added = Cursors.EmplaceGetRef();
                Added.Anchor = { Line, (int32)Found };
                Added.Head = { Line, Found + Needle.Len() };
                if (Added.Anchor == Main.GetRange().Start)
                {
                    MainCursor = Cursors.Num() - 1;
                }
            }
        }
        if (Cursors.IsEmpty())
        {
            Cursors.Add(Main);
            MainCursor = 0;
        }
        NormalizeCursors();
        OnCursorsMoved();
    }

    void FCodeEditor::AddCursorVertical(int32 Direction)
    {
        const int32 TabSize = Math::Max(1, Settings.TabSize);
        int32 Edge = 0;
        for (int32 i = 1; i < Cursors.Num(); ++i)
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
        Cursors.Add(Added);
        MainCursor = Cursors.Num() - 1;
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
        if (Location.Path.IsEmpty() || !DefinitionHandler)
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
            int32 Index = 0;
            int32 Start = 0;
            int32 End = 0;
        };
        TVector<FParsedStop> Parsed;
        FString Text;
        for (int32 i = 0; i < Snippet.Len(); ++i)
        {
            const char C = Snippet[i];
            if (C == '$' && i + 1 < Snippet.Len() && CodeText::IsDigit(Snippet[i + 1]))
            {
                Parsed.Emplace(Snippet[i + 1] - '0', Text.Len(), Text.Len());
                ++i;
                continue;
            }
            if (C == '$' && i + 3 < Snippet.Len() && Snippet[i + 1] == '{' && CodeText::IsDigit(Snippet[i + 2]) && Snippet[i + 3] == ':')
            {
                const int32 Close = Snippet.Find('}', i + 4);
                if (Close != Constants::kIndexNone)
                {
                    const int32 Start = Text.Len();
                    Text += Snippet.Mid(i + 4, Close - (i + 4));
                    Parsed.Emplace(Snippet[i + 2] - '0', Start, Text.Len());
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

        SnippetStops.Reset();
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
            SnippetStops.Emplace(Stop.Index, Range);
        }
        Algo::StableSort(SnippetStops.begin(), SnippetStops.end(), [](const FSnippetStop& A, const FSnippetStop& B) { return A.Index < B.Index; });

        if (SnippetStops.IsEmpty())
        {
            Cursors[0].Anchor = Final;
            Cursors[0].Head = Final;
            return;
        }
        SnippetStops.Add({ 0, { Final, Final } });
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
        if (SnippetCurrent < 0 || SnippetStops.IsEmpty())
        {
            return false;
        }
        SnippetCurrent = Math::Clamp(SnippetCurrent + Direction, 0, SnippetStops.Num() - 1);
        const FSnippetStop Stop = SnippetStops[SnippetCurrent];
        ClearExtraCursors();
        Cursors[0].Anchor = Stop.Range.Start;
        Cursors[0].Head = Stop.Range.End;
        if (Stop.Index == 0)
        {
            SnippetStops.Reset();
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
        Request.Word = Line.Mid(Request.WordStart.Column, Request.Caret.Column - Request.WordStart.Column);
        Request.LinePrefix = Line.Mid(0, Request.Caret.Column);
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
        if (Cursors.Num() != 1 || (Language == nullptr && !CompletionSource.Gather))
        {
            CloseCompletion();
            return;
        }
        const FCodeCompletionRequest Request = MakeCompletionRequest(TriggerChar, bExplicit);
        Completion.Items.Reset();
        if (Language != nullptr)
        {
            Language->GetCompletions(Request, Completion.Items);
        }
        if (CompletionSource.Gather)
        {
            CompletionSource.Gather(Request, Completion.Items);
        }
        if (Completion.Items.IsEmpty())
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
        if (Completion.Visible.IsEmpty())
        {
            CloseCompletion();
        }
    }

    void FCodeEditor::FilterCompletion()
    {
        const FCodePosition Caret = Cursors[MainCursor].Head;
        const FString Word(Document.GetLineView(Caret.Line).Mid(Completion.WordStart.Column, Caret.Column - Completion.WordStart.Column));
        Completion.FilteredCaret = Caret;
        Completion.Visible.Reset();
        for (int32 i = 0; i < Completion.Items.Num(); ++i)
        {
            const FCodeCompletionItem& Item = Completion.Items[i];
            FCompletionEntry Entry;
            Entry.Item = i;
            if (!Word.IsEmpty())
            {
                int32 Score = 0;
                if (!FuzzyMatch(*Word, *Item.Label, Score, Entry.Matches))
                {
                    continue;
                }
                Entry.Score = Score + Item.SortBoost + (Item.Label.StartsWith(Word.View()) ? 25 : 0);
            }
            else
            {
                Entry.Score = Item.SortBoost;
            }
            Completion.Visible.Add(Move(Entry));
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
            if (LabelA.Len() != LabelB.Len())
            {
                return LabelA.Len() < LabelB.Len();
            }
            return LabelA < LabelB;
        });

        // A word already typed out in full leaves nothing to suggest.
        if (Completion.Visible.Num() == 1 && Items[Completion.Visible[0].Item].Label == Word && Items[Completion.Visible[0].Item].InsertText.IsEmpty())
        {
            Completion.Visible.Reset();
        }
        Completion.Selected = Math::Clamp(Completion.Selected, 0, Math::Max(0, Completion.Visible.Num() - 1));
        Completion.bScrollToSelected = true;
    }

    void FCodeEditor::RefreshCompletion()
    {
        if (!Completion.bOpen)
        {
            return;
        }
        const FCodePosition Caret = Cursors[MainCursor].Head;
        if (Cursors.Num() != 1 || Cursors[MainCursor].HasSelection() || Caret.Line != Completion.WordStart.Line || Caret < Completion.WordStart)
        {
            CloseCompletion();
            return;
        }
        const FStringView Typed = Document.GetLineView(Caret.Line).Mid(Completion.WordStart.Column, Caret.Column - Completion.WordStart.Column);
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
        if (!Completion.bOpen || Completion.Visible.IsEmpty())
        {
            CloseCompletion();
            return;
        }
        const FCodeCompletionItem Item = Completion.Items[Completion.Visible[Completion.Selected].Item];
        const FCodeRange Replace{ Completion.WordStart, Cursors[MainCursor].Head };
        CloseCompletion();

        BeginEdit(EEditKind::Other);
        EraseRaw(Replace);
        BeginSnippet(Replace.Start, Item.InsertText.IsEmpty() ? Item.Label.View() : Item.InsertText.View());
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
        if (Language == nullptr || Cursors.Num() != 1)
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
        if (Cursors.Num() != 1)
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
            const int32 Count = Math::Min(Text.Len(), static_cast<int32>(sizeof(Find.Query) - 1));
            memcpy(Find.Query, Text.GetData(), Count);
            Find.Query[Count] = '\0';
        }
        Find.MatchesVersion = ~0ull;
        CloseCompletion();
    }

    void FCodeEditor::UpdateFindMatches()
    {
        const FStringView Query(Find.Query);
        if (!Find.bOpen || Query.IsEmpty())
        {
            Find.Matches.Reset();
            Find.MatchesQuery.Reset();
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
        Find.Matches.Reset();

        auto IsWord = [this](char C)
        {
            return Language != nullptr ? Language->IsWordChar(C) : (CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '_');
        };
        for (int32 Line = 0; Line < Document.GetLineCount() && Find.Matches.Num() < MaxFindMatches; ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            int32 From = 0;
            while (true)
            {
                const int32 Found = Find.bMatchCase ? Text.Find(Query, From) : CodeText::FindIgnoreCase(Text, Query, From);
                if (Found < 0 || Found == (int32)Constants::kIndexNone)
                {
                    break;
                }
                const int32 End = Found + Query.Len();
                const bool bWhole = !Find.bWholeWord || ((Found == 0 || !IsWord(Text[Found - 1])) && (End >= Text.Len() || !IsWord(Text[End])));
                if (bWhole)
                {
                    Find.Matches.Add({ { Line, Found }, { Line, End } });
                }
                From = Found + 1;
            }
        }
    }

    bool FCodeEditor::FindNext(bool bBackwards)
    {
        UpdateFindMatches();
        if (Find.Matches.IsEmpty())
        {
            return false;
        }
        const FCodeRange Selection = Cursors[MainCursor].GetRange();
        const FCodeRange* Target = nullptr;
        if (bBackwards)
        {
            for (int32 i = Find.Matches.Num() - 1; i >= 0 && Target == nullptr; --i)
            {
                Target = Find.Matches[i].Start < Selection.Start ? &Find.Matches[i] : nullptr;
            }
            Target = Target != nullptr ? Target : &Find.Matches.Last();
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
            Target = Target != nullptr ? Target : &Find.Matches[0];
        }
        Select(*Target, true);
        return true;
    }

    int32 FCodeEditor::ReplaceAll()
    {
        UpdateFindMatches();
        if (Settings.bReadOnly || Find.Matches.IsEmpty())
        {
            return 0;
        }
        const TVector<FCodeRange> Matches = Find.Matches;
        BeginEdit(EEditKind::Other);
        for (int32 i = Matches.Num() - 1; i >= 0; --i)
        {
            ReplaceRange(Matches[i], FStringView(Find.Replacement));
        }
        EndEdit();
        return Matches.Num();
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
        if (LanguageDiagnostics.Num() > MaxDiagnostics)
        {
            LanguageDiagnostics.SetNum(MaxDiagnostics);
        }
    }

    void FCodeEditor::ApplyEdits(TSpan<const FCodeTextEdit> Edits)
    {
        if (Settings.bReadOnly)
        {
            return;
        }
        TVector<const FCodeTextEdit*> Local;
        for (const FCodeTextEdit& Edit : Edits)
        {
            if (Edit.Path.IsEmpty())
            {
                Local.Add(&Edit);
            }
        }
        if (Local.IsEmpty())
        {
            return;
        }
        // Last first, so an edit never moves the ranges of the ones still to come.
        Algo::Sort(Local, [](const FCodeTextEdit* A, const FCodeTextEdit* B) { return B->Range.Start < A->Range.Start; });
        BeginEdit(EEditKind::Other);
        for (const FCodeTextEdit* Edit : Local)
        {
            ReplaceRange(Edit->Range, Edit->Text.View());
        }
        EndEdit();
    }

    void FCodeEditor::FormatDocument()
    {
        if (Settings.bReadOnly || Language == nullptr)
        {
            return;
        }
        const int32 TabSize = Math::Max(1, Settings.TabSize);
        Document.Highlight(*Language, Document.GetLineCount() - 1);
        auto EndsStatement = [](FStringView Text)
        {
            const FStringView Trimmed = Text.Mid(0, Text.FindLastNotOf(" \t") + 1);
            if (Trimmed.IsEmpty())
            {
                return true;
            }
            const char Last = Trimmed.Last();
            return Last == ';' || Last == '{' || Last == '}' || Last == ']' || Last == ':' || Last == ',' || Trimmed[0] == '#'
                || Trimmed.StartsWith("//") || Trimmed.EndsWith("*/");
        };

        struct FLineEdit
        {
            int32   Line = 0;
            int32   OldIndent = 0;
            FString NewIndent;
        };
        TVector<FLineEdit> Changes;
        int32 Depth = 0;
        // One flag per open brace, true when that block is a switch body whose case labels push their statements in one more step.
        TVector<bool> SwitchBlocks;
        bool bInCase = false;
        int32 StatementShift = 0;
        bool bPreviousEnded = true;
        bool bNextIsSwitch = false;
        for (int32 Line = 0; Line < Document.GetLineCount(); ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            const TVector<ECodeToken>& Tokens = Document.GetTokens(Line);
            const int32 Indent = CodeText::LeadingWhitespace(Text);
            const FStringView Trimmed = Text.Mid(Indent);
            const bool bContinuesBlock = Document.GetStartState(*Language, Line) != Language->GetInitialState();
            const bool bTokens = Tokens.Num() == Text.Len();

            int32 Opens = 0;
            int32 Closes = 0;
            int32 LeadingCloses = 0;
            bool bLeading = true;
            for (int32 Column = Indent; bTokens && Column < Text.Len(); ++Column)
            {
                const bool bBrace = Tokens[Column] == ECodeToken::Punctuation && (Text[Column] == '{' || Text[Column] == '}');
                if (bBrace && Text[Column] == '{')
                {
                    ++Opens;
                    bLeading = false;
                }
                else if (bBrace)
                {
                    ++Closes;
                    LeadingCloses += bLeading ? 1 : 0;
                }
                else if (!CodeText::IsSpace(Text[Column]))
                {
                    bLeading = false;
                }
            }

            const bool bCaseLabel = Trimmed.StartsWith("case ") || Trimmed.StartsWith("default:") || Trimmed.StartsWith("default :");
            const int32 BlockDepth = Math::Max(0, Depth - LeadingCloses);
            const bool bSwitchBody = !SwitchBlocks.IsEmpty() && SwitchBlocks.Last() && LeadingCloses == 0;
            const int32 Target = BlockDepth + (bSwitchBody && bInCase && !bCaseLabel ? 1 : 0);
            if (!CodeText::IsBlank(Text) && !bContinuesBlock && Trimmed[0] != '#')
            {
                const int32 Current = CodeText::VisualColumn(Text, Indent, TabSize);
                int32 Wanted = Target * TabSize;
                if (!bPreviousEnded)
                {
                    // A continuation keeps its offset from the statement it continues, shifted as that statement was.
                    Wanted = Math::Max(0, Current + StatementShift);
                }
                else
                {
                    StatementShift = Wanted - Current;
                }
                FString NewIndent;
                if (Settings.bInsertSpaces)
                {
                    NewIndent.AppendChars(' ', Wanted);
                }
                else
                {
                    NewIndent.AppendChars('\t', Wanted / TabSize);
                    NewIndent.AppendChars(' ', Wanted % TabSize);
                }
                if (FStringView(NewIndent) != Text.Mid(0, Indent))
                {
                    Changes.Emplace(Line, Indent, Move(NewIndent));
                }
                bPreviousEnded = EndsStatement(Text);
            }
            else if (CodeText::IsBlank(Text) && Indent > 0)
            {
                Changes.Emplace(Line, Indent, FString());
            }

            if (bSwitchBody && bCaseLabel)
            {
                bInCase = true;
            }
            for (int32 Close = 0; Close < Closes && Depth > 0; ++Close)
            {
                --Depth;
                if (!SwitchBlocks.IsEmpty())
                {
                    SwitchBlocks.Pop();
                }
                bInCase = false;
            }
            bNextIsSwitch = bNextIsSwitch || Trimmed.StartsWith("switch");
            for (int32 Open = 0; Open < Opens; ++Open)
            {
                ++Depth;
                SwitchBlocks.Add(bNextIsSwitch);
                bNextIsSwitch = false;
                bInCase = false;
            }
        }
        if (Changes.IsEmpty())
        {
            return;
        }
        BeginEdit(EEditKind::Other);
        for (const FLineEdit& Change : Changes)
        {
            ReplaceRange({ { Change.Line, 0 }, { Change.Line, Change.OldIndent } }, Change.NewIndent.View());
        }
        EndEdit();
    }

    void FCodeEditor::ShowCodeActions()
    {
        CodeActions.Reset();
        if (Language == nullptr || Settings.bReadOnly)
        {
            return;
        }
        Language->GetCodeActions(Document, Cursors[MainCursor].Head, CodeActions);
        bOpenCodeActions = true;
    }

    void FCodeEditor::UpdateFolds()
    {
        constexpr double FoldSettleSeconds = 0.25;
        if (Language == nullptr || FoldsVersion == Document.GetVersion() || Now() - LastEditTime < FoldSettleSeconds)
        {
            return;
        }
        FoldsVersion = Document.GetVersion();
        Document.Highlight(*Language, Document.GetLineCount() - 1);
        TVector<FCodeFold> Found;
        Language->GetFoldingRanges(Document, Found);
        Folds = Move(Found);
        const int32 Before = Collapsed.Num();
        Collapsed.RemoveAll([this](int32 Line) { return FoldStartingAt(Line) == nullptr; });
        if (Collapsed.Num() != Before)
        {
            ++CollapseSerial;
        }
    }

    const FCodeFold* FCodeEditor::FoldStartingAt(int32 Line) const
    {
        const auto Found = std::lower_bound(Folds.begin(), Folds.end(), Line, [](const FCodeFold& Fold, int32 Start) { return Fold.StartLine < Start; });
        return Found != Folds.end() && Found->StartLine == Line ? &*Found : nullptr;
    }

    bool FCodeEditor::IsCollapsed(int32 Line) const
    {
        return std::binary_search(Collapsed.begin(), Collapsed.end(), Line);
    }

    void FCodeEditor::RebuildRows()
    {
        const int32 LineCount = Document.GetLineCount();
        if (RowsVersion == Document.GetVersion() && RowsCollapseSerial == CollapseSerial && RowOfLine.Num() == LineCount)
        {
            return;
        }
        RowsVersion = Document.GetVersion();
        RowsCollapseSerial = CollapseSerial;
        RowOfLine.SetNum(LineCount);
        LineOfRow.Reset();
        int32 HiddenThrough = -1;
        for (int32 Line = 0; Line < LineCount; ++Line)
        {
            if (Line <= HiddenThrough)
            {
                RowOfLine[Line] = LineOfRow.Num() - 1;
                continue;
            }
            RowOfLine[Line] = LineOfRow.Num();
            LineOfRow.Add(Line);
            if (IsCollapsed(Line))
            {
                if (const FCodeFold* Fold = FoldStartingAt(Line))
                {
                    HiddenThrough = Math::Max(HiddenThrough, Math::Min(Fold->EndLine, LineCount - 1));
                }
            }
        }
    }

    int32 FCodeEditor::RowOf(int32 Line) const
    {
        if (RowOfLine.IsEmpty())
        {
            return Line;
        }
        return RowOfLine[Math::Clamp(Line, 0, RowOfLine.Num() - 1)];
    }

    int32 FCodeEditor::LineAtRow(int32 Row) const
    {
        if (LineOfRow.IsEmpty())
        {
            return Math::Clamp(Row, 0, Document.GetLineCount() - 1);
        }
        return LineOfRow[Math::Clamp(Row, 0, LineOfRow.Num() - 1)];
    }

    bool FCodeEditor::IsLineHidden(int32 Line) const
    {
        if (Line < 0 || Line >= RowOfLine.Num())
        {
            return false;
        }
        const int32 Row = RowOfLine[Line];
        return Row >= 0 && Row < LineOfRow.Num() && LineOfRow[Row] != Line;
    }

    void FCodeEditor::RevealLine(int32 Line)
    {
        bool bOpened = false;
        for (int32 Index = 0; Index < Collapsed.Num();)
        {
            const FCodeFold* Fold = FoldStartingAt(Collapsed[Index]);
            if (Fold != nullptr && Fold->StartLine < Line && Line <= Fold->EndLine)
            {
                Collapsed.RemoveAt(Index);
                bOpened = true;
                continue;
            }
            ++Index;
        }
        if (bOpened)
        {
            ++CollapseSerial;
        }
    }

    void FCodeEditor::ToggleFold(int32 Line)
    {
        const FCodeFold* Fold = FoldStartingAt(Line);
        if (Fold == nullptr)
        {
            return;
        }
        const int32 FoundIndex = static_cast<int32>(std::lower_bound(Collapsed.begin(), Collapsed.end(), Line) - Collapsed.begin());
        if (FoundIndex < Collapsed.Num() && Collapsed[FoundIndex] == Line)
        {
            Collapsed.RemoveAt(FoundIndex);
        }
        else
        {
            Collapsed.Insert(Line, FoundIndex);
            // A caret inside the block moves to its first line rather than holding the fold open.
            for (FCursor& Cursor : Cursors)
            {
                if (Cursor.Head.Line > Line && Cursor.Head.Line <= Fold->EndLine)
                {
                    Cursor.Head = Cursor.Anchor = { Line, Document.GetLine(Line).Len() };
                }
            }
        }
        ++CollapseSerial;
    }

    void FCodeEditor::FoldAll()
    {
        Collapsed.Reset();
        for (const FCodeFold& Fold : Folds)
        {
            Collapsed.Add(Fold.StartLine);
        }
        ClearExtraCursors();
        Cursors[0].Head = Cursors[0].Anchor = {};
        ++CollapseSerial;
    }

    void FCodeEditor::UnfoldAll()
    {
        Collapsed.Reset();
        ++CollapseSerial;
    }

    void FCodeEditor::ShiftFoldsForInsert(FCodePosition At, FCodePosition End)
    {
        const int32 Added = End.Line - At.Line;
        if (Added == 0)
        {
            return;
        }
        for (FCodeFold& Fold : Folds)
        {
            Fold.StartLine += Fold.StartLine > At.Line ? Added : 0;
            Fold.EndLine += Fold.EndLine > At.Line || (Fold.EndLine == At.Line && At.Column == 0) ? Added : 0;
        }
        for (int32& Line : Collapsed)
        {
            Line += Line > At.Line ? Added : 0;
        }
        ++CollapseSerial;
    }

    void FCodeEditor::ShiftFoldsForErase(const FCodeRange& Range)
    {
        const int32 Removed = Range.End.Line - Range.Start.Line;
        if (Removed == 0)
        {
            return;
        }
        auto Shift = [&](int32 Line) { return Line > Range.End.Line ? Line - Removed : Line > Range.Start.Line ? Range.Start.Line : Line; };
        Folds.RemoveAll([&](const FCodeFold& Fold) { return Fold.StartLine > Range.Start.Line && Fold.StartLine <= Range.End.Line; });
        for (FCodeFold& Fold : Folds)
        {
            Fold.StartLine = Shift(Fold.StartLine);
            Fold.EndLine = Shift(Fold.EndLine);
        }
        Collapsed.RemoveAll([&](int32 Line) { return Line > Range.Start.Line && Line <= Range.End.Line; });
        for (int32& Line : Collapsed)
        {
            Line = Shift(Line);
        }
        ++CollapseSerial;
    }
}
