#pragma once

#include "CodeDocument.h"
#include "CodeLanguage.h"
#include "Containers/Function.h"
#include "Containers/Span.h"
#include <imgui.h>

namespace Lumina
{
    struct EDITOR_API FCodeEditorPalette
    {
        ImU32 Tokens[(int32)ECodeToken::Count] = {};
        ImU32 Background = 0;
        ImU32 Gutter = 0;
        ImU32 LineNumber = 0;
        ImU32 LineNumberActive = 0;
        ImU32 CurrentLine = 0;
        ImU32 Selection = 0;
        ImU32 Cursor = 0;
        ImU32 Whitespace = 0;
        ImU32 IndentGuide = 0;
        ImU32 BracketMatch = 0;
        ImU32 WordHighlight = 0;
        ImU32 FindMatch = 0;
        ImU32 Error = 0;
        ImU32 Warning = 0;
        ImU32 Info = 0;
        ImU32 PopupBackground = 0;
        ImU32 PopupBorder = 0;
        ImU32 PopupSelected = 0;
        ImU32 PopupMatch = 0;
        ImU32 PopupDetail = 0;
        ImU32 MiniMapViewport = 0;

        ImU32& operator[](ECodeToken Token) { return Tokens[(int32)Token]; }
        ImU32 operator[](ECodeToken Token) const { return Tokens[(int32)Token]; }

        static FCodeEditorPalette Dark();
        static FCodeEditorPalette Light();
    };

    struct FCodeEditorSettings
    {
        float FontScale = 1.0f;
        float LineSpacing = 1.0f;
        int32 TabSize = 4;
        bool  bInsertSpaces = true;
        bool  bShowLineNumbers = true;
        bool  bShowWhitespace = false;
        bool  bShowMiniMap = true;
        bool  bShowIndentGuides = true;
        bool  bReadOnly = false;
        bool  bAutoIndent = true;
        bool  bMatchBrackets = true;
        bool  bAutoClosePairs = true;
        bool  bSuggestWhileTyping = true;
        bool  bHighlightOccurrences = true;
    };

    // Completion the host adds to the language's own, such as the data models a document binds.
    struct FCodeCompletionSource
    {
        TFunction<bool(char Typed, const FCodeCompletionRequest& Request)>                    IsTrigger;
        TFunction<void(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out)> Gather;
    };

    class EDITOR_API FCodeEditor
    {
    public:

        FCodeEditor();

        void Render(const char* Id, const ImVec2& Size, bool bBorder = false);
        void Focus() { bFocusRequested = true; }
        bool IsFocused() const { return bFocused; }

        void SetText(FStringView Text);
        FString GetText() const { return Document.GetText(); }
        const FCodeDocument& GetDocument() const { return Document; }
        int32 GetLineCount() const { return Document.GetLineCount(); }
        uint64 GetVersion() const { return Document.GetVersion(); }
        FString GetSelectedText() const;

        void SetLanguage(const FCodeLanguage* InLanguage);
        const FCodeLanguage* GetLanguage() const { return Language; }
        // Re-colors the document after the language's tables or context symbols changed.
        void InvalidateHighlight();

        FCodeEditorSettings& GetSettings() { return Settings; }
        const FCodeEditorSettings& GetSettings() const { return Settings; }
        FCodeEditorPalette& GetPalette() { return Palette; }
        void SetPalette(const FCodeEditorPalette& InPalette) { Palette = InPalette; }

        void SetCompletionSource(FCodeCompletionSource Source) { CompletionSource = Move(Source); }
        // Receives a definition in another file. Without one, or for this document, the caret jumps there itself.
        void SetDefinitionHandler(TFunction<void(const FCodeLocation& Location)> Handler) { DefinitionHandler = Move(Handler); }
        void SetChangeCallback(TFunction<void()> Callback, float DelaySeconds);
        // Problems the host found, shown beside the language's own.
        void SetExternalDiagnostics(TVector<FCodeDiagnostic> Diagnostics);
        const TVector<FCodeDiagnostic>& GetDiagnostics() const { return LanguageDiagnostics; }

        FCodePosition GetCursor() const { return Cursors[MainCursor].Head; }
        FCodeRange GetSelection() const;
        bool HasSelection() const;
        int32 GetCursorCount() const { return (int32)Cursors.size(); }
        void SetCursor(FCodePosition Position, bool bCenter = true);
        void Select(const FCodeRange& Range, bool bCenter = true);
        void SelectAll();
        void ScrollToLine(int32 Line);

        bool CanUndo() const { return !Settings.bReadOnly && !UndoStack.empty(); }
        bool CanRedo() const { return !Settings.bReadOnly && !RedoStack.empty(); }
        void Undo();
        void Redo();

        void InsertText(FStringView Text);
        void InsertSnippet(FStringView Snippet);
        void TypeCharacter(uint32 Codepoint);
        void NewLine();
        void Backspace(bool bWord);
        void Delete(bool bWord);
        void Cut();
        void Copy() const;
        void Paste();
        void Indent();
        void Outdent();
        void MoveLines(int32 Direction);
        void DuplicateLines(int32 Direction);
        void DeleteLines();
        void InsertLine(bool bAbove);
        void ToggleLineComment();
        void ToggleBlockComment();
        void ChangeCase(bool bUpper);
        void TrimTrailingWhitespace();
        void ConvertIndentation(bool bToSpaces);

        void AddNextOccurrence();
        void SelectAllOccurrences();
        void AddCursorVertical(int32 Direction);
        void JumpToMatchingBracket();
        bool GoToDefinition(FCodePosition Position);

        void OpenFind(bool bReplace);
        bool FindNext(bool bBackwards);
        int32 ReplaceAll();

        void TriggerCompletion();
        void TriggerSignatureHelp();

        // Applies replacements to this document as one undo step. An edit naming another file is the host's to apply.
        void ApplyEdits(TSpan<const FCodeTextEdit> Edits);
        // Re-indents every line from the braces around it, moving continuation lines with the statement they belong to.
        void FormatDocument();
        // Asks the language for fixes at the caret and lists them beside it.
        void ShowCodeActions();

        void ToggleFold(int32 Line);
        void FoldAll();
        void UnfoldAll();
        bool IsLineHidden(int32 Line) const;
        const TVector<FCodeFold>& GetFolds() const { return Folds; }

        // Commands the editor offers but the host carries out, since they reach across files.
        enum class ECommand : uint8
        {
            FindReferences,
            Rename,
        };
        void SetCommandHandler(TFunction<void(ECommand, FCodePosition)> Handler) { CommandHandler = Move(Handler); }

        enum class EMove : uint8
        {
            Left,
            Right,
            WordLeft,
            WordRight,
            Up,
            Down,
            PageUp,
            PageDown,
            LineStart,
            LineEnd,
            DocumentStart,
            DocumentEnd,
        };
        void MoveCursors(EMove Move, bool bSelect);

    private:

        struct FCursor
        {
            FCodePosition Anchor;
            FCodePosition Head;
            // The visual column vertical moves aim for, so passing a short line does not lose it.
            int32         PreferredVisual = -1;

            FCodeRange GetRange() const { return Anchor < Head ? FCodeRange{ Anchor, Head } : FCodeRange{ Head, Anchor }; }
            bool HasSelection() const { return Anchor != Head; }
            bool operator==(const FCursor& Other) const { return Anchor == Other.Anchor && Head == Other.Head; }
        };

        enum class EEditKind : uint8
        {
            Other,
            Typing,
            Deleting,
        };

        struct FEditOp
        {
            FCodePosition Start;
            FCodePosition End;
            FString       Text;
            bool          bInsert = true;
        };

        struct FUndoRecord
        {
            TVector<FEditOp> Ops;
            TVector<FCursor> Before;
            TVector<FCursor> After;
            EEditKind        Kind = EEditKind::Other;
            double           Time = 0.0;
        };

        struct FSnippetStop
        {
            int32      Index = 0;
            FCodeRange Range;
        };

        struct FCompletionEntry
        {
            int32           Item = 0;
            int32           Score = 0;
            TVector<uint16> Matches;
        };

        struct FCompletionState
        {
            bool                         bOpen = false;
            FCodePosition                WordStart;
            TVector<FCodeCompletionItem> Items;
            TVector<FCompletionEntry>    Visible;
            int32                        Selected = 0;
            int32                        FirstRow = 0;
            bool                         bScrollToSelected = false;
            uint64                       FilteredVersion = ~0ull;
            FCodePosition                FilteredCaret;
        };

        struct FFindState
        {
            bool                bOpen = false;
            bool                bReplace = false;
            bool                bMatchCase = false;
            bool                bWholeWord = false;
            bool                bFocusQuery = false;
            char                Query[256] = {};
            char                Replacement[256] = {};
            TVector<FCodeRange> Matches;
            uint64              MatchesVersion = ~0ull;
            FString             MatchesQuery;
            bool                bMatchesCase = false;
            bool                bMatchesWord = false;
        };

        struct FHoverState
        {
            ImVec2        MousePos;
            double        StillSince = 0.0;
            bool          bQueried = false;
            bool          bValid = false;
            FCodeHover    Hover;
            FString       DiagnosticText;
            ImU32         DiagnosticColor = 0;
        };

        struct FColorPick
        {
            bool       bRequestOpen = false;
            FCodeRange Range;
            float      Color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        };

        struct FLayout
        {
            ImFont* Font = nullptr;
            float   FontSize = 0.0f;
            float   CharWidth = 0.0f;
            float   LineHeight = 0.0f;
            float   GutterWidth = 0.0f;
            float   TextOffset = 0.0f;
            float   MiniMapWidth = 0.0f;
            ImVec2  WindowPos;
            ImVec2  ViewMin;
            ImVec2  ViewMax;
            float   ScrollX = 0.0f;
            float   ScrollY = 0.0f;
            float   ContentHeight = 0.0f;
            int32   FirstLine = 0;
            int32   LastLine = 0;
            int32   MiniMapFirstLine = 0;
            float   MiniMapRowHeight = 2.0f;
        };

        static double Now();
        void ResetEditState();

        // Every document change runs through these two, which record undo and shift every cursor and tab stop.
        FCodePosition InsertRaw(FCodePosition At, FStringView Text);
        void EraseRaw(const FCodeRange& Range);
        void ReplaceRange(const FCodeRange& Range, FStringView Text);

        void BeginEdit(EEditKind Kind);
        void EndEdit();
        void ApplyUndoRecord(const FUndoRecord& Record, bool bUndo);

        void NormalizeCursors();
        void ClearExtraCursors();
        void OnCursorsMoved();
        void EnsureCursorVisible() { bEnsureCursorVisible = true; }
        void DeleteSelections();
        FCodePosition MovePosition(FCodePosition Position, EMove Move, int32& PreferredVisual) const;
        FCodePosition WordBoundary(FCodePosition Position, bool bForward) const;
        FCodeRange WordAt(FCodePosition Position) const;
        bool FindBracketPair(FCodePosition Caret, FCodePosition& OutOpen, FCodePosition& OutClose);
        TVector<int32> SelectedLines() const;
        FString IndentUnit() const;
        FString LineIndent(int32 Line) const;
        uint32 StateAt(FCodePosition Position);

        void TypeAtCursors(FStringView Text);
        // Indents a multi-line selection, otherwise pads each caret out to the next tab stop.
        void InsertTab();
        bool TryOvertypeCloser(char Typed);
        bool TryAutoClosePair(char Typed);
        void ApplyAutoInsert(char Typed);
        void OutdentLineIfTriggered(char Typed);

        void BeginSnippet(FCodePosition At, FStringView Snippet);
        bool AdvanceSnippet(int32 Direction);

        FCodeCompletionRequest MakeCompletionRequest(char TriggerChar, bool bExplicit);
        void OpenCompletion(char TriggerChar, bool bExplicit);
        void RefreshCompletion();
        void FilterCompletion();
        void AcceptCompletion();
        void CloseCompletion() { Completion.bOpen = false; }
        void MaybeTriggerAfterTyping(char Typed);

        void UpdateFindMatches();
        void UpdateDiagnostics();
        void RefreshSignature();

        void ComputeLayout(const ImVec2& Size);
        void HandleKeyboard();
        void HandleMouse();
        void HandleCompletionKeys(bool& bConsumed);
        FCodePosition PositionAtScreen(const ImVec2& Screen, bool bRoundToNearest) const;
        ImVec2 ScreenAtPosition(FCodePosition Position) const;
        float LineTop(int32 Line) const;

        void DrawFindBar(float Width);
        void DrawBackgrounds(ImDrawList* DrawList);
        void DrawTextRuns(ImDrawList* DrawList);
        void DrawDiagnostics(ImDrawList* DrawList);
        void DrawCursors(ImDrawList* DrawList);
        void DrawGutter(ImDrawList* DrawList);
        void DrawMiniMap(ImDrawList* DrawList);
        void DrawCompletionPopup();
        void DrawSignaturePopup();
        void DrawHoverPopup();
        void DrawColorPicker();
        void DrawContextMenu();
        void DrawCodeActionsPopup();
        void UpdateMaxLineColumns();

        void UpdateFolds();
        void RebuildRows();
        int32 RowOf(int32 Line) const;
        int32 LineAtRow(int32 Row) const;
        int32 VisibleRows() const { return Math::Max(1, (int32)LineOfRow.size()); }
        const FCodeFold* FoldStartingAt(int32 Line) const;
        bool IsCollapsed(int32 Line) const;
        // Opens every fold hiding Line, so a caret or a jump never lands out of sight.
        void RevealLine(int32 Line);
        void ShiftFoldsForInsert(FCodePosition At, FCodePosition End);
        void ShiftFoldsForErase(const FCodeRange& Range);
        void DrawSquiggle(ImDrawList* DrawList, float X0, float X1, float Y, ImU32 Color) const;
        ImU32 SeverityColor(ECodeSeverity Severity) const;
        bool PickColorAt(FCodePosition Position);

        FCodeDocument                Document;
        const FCodeLanguage*         Language = nullptr;
        FCodeEditorSettings          Settings;
        FCodeEditorPalette           Palette;
        FCodeCompletionSource        CompletionSource;

        TVector<FCursor>             Cursors;
        int32                        MainCursor = 0;

        TVector<FUndoRecord>         UndoStack;
        TVector<FUndoRecord>         RedoStack;
        FUndoRecord                  PendingEdit;
        int32                        EditDepth = 0;
        bool                         bReplaying = false;

        TVector<FSnippetStop>        SnippetStops;
        int32                        SnippetCurrent = -1;

        FCompletionState             Completion;
        FFindState                   Find;
        FHoverState                  HoverState;
        FColorPick                   ColorPick;

        bool                         bSignatureOpen = false;
        FCodeSignature               Signature;
        FCodePosition                SignatureCaret;

        TVector<FCodeDiagnostic>     LanguageDiagnostics;
        TVector<FCodeDiagnostic>     ExternalDiagnostics;
        uint64                       DiagnosticsVersion = ~0ull;
        double                       LastEditTime = 0.0;

        double                       DiagnosticsNotBefore = 0.0;
        TFunction<void(const FCodeLocation&)> DefinitionHandler;

        TFunction<void()>            ChangeCallback;
        float                        ChangeDelay = 0.0f;
        uint64                       ReportedVersion = 0;
        bool                         bChangePending = false;

        FLayout                      Layout;
        int32                        MaxLineColumns = 0;
        uint64                       MaxLineColumnsVersion = ~0ull;
        int32                        MaxLineColumnsTabSize = 0;
        bool                         bCompletionHovered = false;

        double                       CursorBlinkStart = 0.0;
        bool                         bFocused = false;
        bool                         bFocusRequested = false;
        bool                         bEnsureCursorVisible = false;
        int32                        CenterLineRequest = -1;

        bool                         bSelectingWithMouse = false;
        bool                         bSelectingWords = false;
        bool                         bSelectingLines = false;
        bool                         bDraggingMiniMap = false;
        FCodeRange                   MouseSelectOrigin;
        double                       LastClickTime = 0.0;
        FCodePosition                LastClickPosition;
        int32                        ClickCount = 0;
        FCodePosition                ContextMenuPosition;

        TVector<FCodeFold>           Folds;
        uint64                       FoldsVersion = ~0ull;
        // Start lines of the folds the reader collapsed, kept sorted.
        TVector<int32>               Collapsed;
        uint32                       CollapseSerial = 0;
        TVector<int32>               RowOfLine;
        TVector<int32>               LineOfRow;
        uint64                       RowsVersion = ~0ull;
        uint32                       RowsCollapseSerial = ~0u;

        TVector<FCodeAction>         CodeActions;
        bool                         bOpenCodeActions = false;
        TFunction<void(ECommand, FCodePosition)> CommandHandler;
    };
}
