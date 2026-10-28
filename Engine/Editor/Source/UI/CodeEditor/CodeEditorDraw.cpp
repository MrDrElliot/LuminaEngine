#include "CodeEditor.h"

#include <cmath>
#include "Settings/EditorSettings.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"

namespace Lumina
{
    namespace CodeEditorDrawInternal
    {
        constexpr int32  MaxCompletionRows = 10;
        constexpr double HoverDelay = 0.45;
        constexpr float  CursorBlinkPeriod = 1.1f;

        const char* KindIcon(ECodeCompletionKind Kind)
        {
            switch (Kind)
            {
                case ECodeCompletionKind::Keyword:   return LE_ICON_KEY;
                case ECodeCompletionKind::Type:      return LE_ICON_CUBE_OUTLINE;
                case ECodeCompletionKind::Function:  return LE_ICON_FUNCTION_VARIANT;
                case ECodeCompletionKind::Variable:  return LE_ICON_VARIABLE;
                case ECodeCompletionKind::Field:     return LE_ICON_VARIABLE;
                case ECodeCompletionKind::Property:  return LE_ICON_TEXT_BOX_OUTLINE;
                case ECodeCompletionKind::Value:     return LE_ICON_ALPHA_A_BOX_OUTLINE;
                case ECodeCompletionKind::Unit:      return LE_ICON_RULER;
                case ECodeCompletionKind::Color:     return LE_ICON_PALETTE_OUTLINE;
                case ECodeCompletionKind::Element:   return LE_ICON_CODE_TAGS;
                case ECodeCompletionKind::Attribute: return LE_ICON_TAG_OUTLINE;
                case ECodeCompletionKind::Selector:  return LE_ICON_POUND;
                case ECodeCompletionKind::Event:     return LE_ICON_LIGHTNING_BOLT;
                case ECodeCompletionKind::Snippet:   return LE_ICON_CODE_BRACES;
                case ECodeCompletionKind::Module:    return LE_ICON_DATABASE_OUTLINE;
                default:                             return LE_ICON_TEXT;
            }
        }

        ImU32 KindColor(const FCodeEditorPalette& Palette, ECodeCompletionKind Kind)
        {
            switch (Kind)
            {
                case ECodeCompletionKind::Keyword:   return Palette[ECodeToken::Keyword];
                case ECodeCompletionKind::Type:      return Palette[ECodeToken::Type];
                case ECodeCompletionKind::Function:  return Palette[ECodeToken::Function];
                case ECodeCompletionKind::Variable:
                case ECodeCompletionKind::Field:     return Palette[ECodeToken::KnownIdentifier];
                case ECodeCompletionKind::Property:  return Palette[ECodeToken::Property];
                case ECodeCompletionKind::Value:     return Palette[ECodeToken::Value];
                case ECodeCompletionKind::Unit:      return Palette[ECodeToken::Number];
                case ECodeCompletionKind::Element:   return Palette[ECodeToken::Tag];
                case ECodeCompletionKind::Attribute: return Palette[ECodeToken::Attribute];
                case ECodeCompletionKind::Selector:  return Palette[ECodeToken::Selector];
                case ECodeCompletionKind::Event:
                case ECodeCompletionKind::Module:    return Palette[ECodeToken::Binding];
                default:                             return Palette.PopupDetail;
            }
        }

        void PushUIFont()
        {
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Medium);
        }
    }

    using namespace CodeEditorDrawInternal;

    FCodeEditorPalette FCodeEditorPalette::Dark()
    {
        FCodeEditorPalette P;
        P[ECodeToken::Default]         = IM_COL32(214, 216, 222, 255);
        P[ECodeToken::Keyword]         = IM_COL32(197, 134, 222, 255);
        P[ECodeToken::Type]            = IM_COL32(86, 196, 200, 255);
        P[ECodeToken::Function]        = IM_COL32(226, 204, 142, 255);
        P[ECodeToken::KnownIdentifier] = IM_COL32(150, 206, 254, 255);
        P[ECodeToken::Number]          = IM_COL32(181, 214, 160, 255);
        P[ECodeToken::String]          = IM_COL32(214, 152, 118, 255);
        P[ECodeToken::Comment]         = IM_COL32(110, 140, 112, 255);
        P[ECodeToken::Punctuation]     = IM_COL32(150, 156, 172, 255);
        P[ECodeToken::Preprocessor]    = IM_COL32(160, 160, 226, 255);
        P[ECodeToken::Tag]             = IM_COL32(98, 160, 236, 255);
        P[ECodeToken::Attribute]       = IM_COL32(150, 206, 254, 255);
        P[ECodeToken::Property]        = IM_COL32(112, 192, 255, 255);
        P[ECodeToken::Selector]        = IM_COL32(220, 190, 128, 255);
        P[ECodeToken::Value]           = IM_COL32(214, 152, 118, 255);
        P[ECodeToken::Binding]         = IM_COL32(236, 184, 96, 255);
        P.Background       = IM_COL32(22, 24, 28, 255);
        P.Gutter           = IM_COL32(22, 24, 28, 255);
        P.LineNumber       = IM_COL32(86, 90, 104, 255);
        P.LineNumberActive = IM_COL32(196, 200, 212, 255);
        P.CurrentLine      = IM_COL32(255, 255, 255, 9);
        P.Selection        = IM_COL32(66, 150, 250, 72);
        P.Cursor           = IM_COL32(120, 176, 255, 255);
        P.Whitespace       = IM_COL32(255, 255, 255, 38);
        P.IndentGuide      = IM_COL32(255, 255, 255, 16);
        P.BracketMatch     = IM_COL32(66, 150, 250, 120);
        P.WordHighlight    = IM_COL32(255, 255, 255, 22);
        P.FindMatch        = IM_COL32(242, 190, 76, 72);
        P.Error            = IM_COL32(245, 92, 97, 255);
        P.Warning          = IM_COL32(242, 191, 77, 255);
        P.Info             = IM_COL32(102, 170, 250, 255);
        P.PopupBackground  = IM_COL32(30, 32, 38, 252);
        P.PopupBorder      = IM_COL32(64, 68, 82, 255);
        P.PopupSelected    = IM_COL32(66, 150, 250, 64);
        P.PopupMatch       = IM_COL32(120, 182, 255, 255);
        P.PopupDetail      = IM_COL32(128, 132, 146, 255);
        P.MiniMapViewport  = IM_COL32(255, 255, 255, 18);
        return P;
    }

    void FCodeEditor::Render(const char* Id, const ImVec2& Size, bool bBorder)
    {
        ReloadEditedTheme();
        ImGui::PushID(Id);
        const ImVec2 Avail = ImGui::GetContentRegionAvail();
        ImVec2 RenderSize = ImGui::CalcItemSize(Size, Avail.x, Avail.y);
        if (Find.bOpen)
        {
            const float Before = ImGui::GetCursorPosY();
            DrawFindBar(RenderSize.x);
            RenderSize.y -= ImGui::GetCursorPosY() - Before;
        }
        RenderSize.y = Math::Max(RenderSize.y, 8.0f);

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Mono);
        ImGui::PushFontSize(ImGui::GetStyle().FontSizeBase * Math::Clamp(Settings.FontScale, 0.5f, 4.0f));
        ComputeLayout(RenderSize);
        UpdateMaxLineColumns();

        const float LineHeight = Layout.LineHeight;
        const float ScrollbarSize = ImGui::GetStyle().ScrollbarSize;
        const float ViewHeight = RenderSize.y - ScrollbarSize;
        const float TextViewWidth = RenderSize.x - Layout.TextOffset - Layout.MiniMapWidth - ScrollbarSize;
        UpdateFolds();
        RebuildRows();
        Layout.ContentHeight = VisibleRows() * LineHeight + Math::Max(0.0f, ViewHeight - LineHeight * 3.0f);

        ImVec2 Scroll(-1.0f, -1.0f);
        const FCodePosition Caret = Cursors[MainCursor].Head;
        const float CaretX = CodeText::VisualColumn(Document.GetLineView(Caret.Line), Caret.Column, Math::Max(1, Settings.TabSize)) * Layout.CharWidth;
        if (CenterLineRequest >= 0)
        {
            RevealLine(CenterLineRequest);
            RebuildRows();
            Scroll.y = Math::Max(0.0f, RowOf(CenterLineRequest) * LineHeight - ViewHeight * 0.5f + LineHeight * 0.5f);
            CenterLineRequest = -1;
            bEnsureCursorVisible = true;
        }
        if (bEnsureCursorVisible)
        {
            const float CaretTop = RowOf(Caret.Line) * LineHeight;
            const float ScrollY = Scroll.y >= 0.0f ? Scroll.y : Layout.ScrollY;
            if (CaretTop < ScrollY)
            {
                Scroll.y = Math::Max(0.0f, CaretTop - LineHeight);
            }
            else if (CaretTop + LineHeight * 2.0f > ScrollY + ViewHeight)
            {
                Scroll.y = CaretTop + LineHeight * 2.0f - ViewHeight;
            }
            if (CaretX < Layout.ScrollX)
            {
                Scroll.x = Math::Max(0.0f, CaretX - Layout.CharWidth * 4.0f);
            }
            else if (CaretX > Layout.ScrollX + TextViewWidth - Layout.CharWidth * 2.0f)
            {
                Scroll.x = CaretX - TextViewWidth + Layout.CharWidth * 4.0f;
            }
            bEnsureCursorVisible = false;
        }
        if (Scroll.x >= 0.0f || Scroll.y >= 0.0f)
        {
            ImGui::SetNextWindowScroll(Scroll);
        }
        if (bFocusRequested)
        {
            ImGui::SetNextWindowFocus();
            bFocusRequested = false;
        }

        const float ContentWidth = Layout.TextOffset + MaxLineColumns * Layout.CharWidth + Layout.CharWidth * 8.0f + Layout.MiniMapWidth;
        ImGui::SetNextWindowContentSize(ImVec2(ContentWidth, Layout.ContentHeight));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Palette.Background);
        constexpr ImGuiWindowFlags ChildFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoScrollWithMouse;
        if (ImGui::BeginChild("##code", RenderSize, bBorder ? ImGuiChildFlags_Borders : ImGuiChildFlags_None, ChildFlags))
        {
            ImGuiWindow* Window = ImGui::GetCurrentWindow();
            bFocused = ImGui::IsWindowFocused();
            Layout.WindowPos = Window->Pos;
            Layout.ScrollX = Window->Scroll.x;
            Layout.ScrollY = Window->Scroll.y;
            Layout.ViewMin = Window->InnerRect.Min;
            Layout.ViewMax = Window->InnerRect.Max;

            if (!bFocused && Completion.bOpen && !bCompletionHovered)
            {
                CloseCompletion();
            }

            HandleKeyboard();
            HandleMouse();

            // Input above can add or remove lines and folds, so the visible range is taken from the document as it now stands.
            RebuildRows();
            Layout.FirstLine = LineAtRow(Math::Max(0, (int32)(Layout.ScrollY / LineHeight)));
            Layout.LastLine = Math::Min(Document.GetLineCount() - 1, LineAtRow((int32)((Layout.ScrollY + (Layout.ViewMax.y - Layout.ViewMin.y)) / LineHeight)));
            UpdateFindMatches();
            RefreshCompletion();
            UpdateDiagnostics();
            if (Language != nullptr)
            {
                Document.Highlight(*Language, Layout.LastLine);
            }

            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            DrawList->PushClipRect(ImVec2(Layout.ViewMin.x + Layout.GutterWidth, Layout.ViewMin.y), ImVec2(Layout.ViewMax.x - Layout.MiniMapWidth, Layout.ViewMax.y), true);
            DrawBackgrounds(DrawList);
            DrawTextRuns(DrawList);
            DrawDiagnostics(DrawList);
            DrawCursors(DrawList);
            DrawList->PopClipRect();
            DrawGutter(DrawList);
            DrawMiniMap(DrawList);
            DrawContextMenu();
            DrawCodeActionsPopup();
            DrawColorPicker();

            if (bFocused)
            {
                ImGuiPlatformImeData& Ime = ImGui::GetCurrentContext()->PlatformImeData;
                Ime.WantVisible = true;
                Ime.WantTextInput = true;
                Ime.InputPos = ScreenAtPosition(Cursors[MainCursor].Head);
                Ime.InputLineHeight = LineHeight;
                Ime.ViewportId = Window->Viewport->ID;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        DrawCompletionPopup();
        DrawSignaturePopup();
        DrawHoverPopup();

        ImGui::PopFontSize();
        ImGuiX::Font::PopFont();

        if (Document.GetVersion() != ReportedVersion && Now() - LastEditTime >= ChangeDelay)
        {
            ReportedVersion = Document.GetVersion();
            if (ChangeCallback)
            {
                ChangeCallback();
            }
        }
        ImGui::PopID();
    }

    void FCodeEditor::ComputeLayout(const ImVec2& Size)
    {
        Layout.Font = ImGui::GetFont();
        Layout.FontSize = ImGui::GetFontSize();
        // ImGui::CalcTextSize rounds up to whole pixels, which drifts every run off the grid at a fractional advance.
        Layout.CharWidth = Layout.Font->CalcTextSizeA(Layout.FontSize, FLT_MAX, 0.0f, "M").x;
        Layout.LineHeight = std::floor(Layout.FontSize * (1.25f + Math::Clamp(Settings.LineSpacing, 1.0f, 2.0f) - 1.0f) + 2.0f);

        int32 Digits = 1;
        for (int32 Count = Document.GetLineCount(); Count >= 10; Count /= 10)
        {
            ++Digits;
        }
        Digits = Math::Max(3, Digits);
        // The last column and a half hold the fold markers.
        Layout.GutterWidth = (Settings.bShowLineNumbers ? (Digits + 2.5f) * Layout.CharWidth : Layout.CharWidth * 1.5f) + Layout.CharWidth * 1.5f;
        Layout.TextOffset = Layout.GutterWidth + Layout.CharWidth * 0.75f;
        Layout.MiniMapWidth = Settings.bShowMiniMap ? Math::Clamp(Size.x * 0.1f, 48.0f, 96.0f) : 0.0f;
    }

    void FCodeEditor::UpdateMaxLineColumns()
    {
        const int32 TabSize = Math::Max(1, Settings.TabSize);
        if (MaxLineColumnsVersion == Document.GetVersion() && MaxLineColumnsTabSize == TabSize)
        {
            return;
        }
        MaxLineColumnsVersion = Document.GetVersion();
        MaxLineColumnsTabSize = TabSize;
        MaxLineColumns = 0;
        for (int32 Line = 0; Line < Document.GetLineCount(); ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            MaxLineColumns = Math::Max(MaxLineColumns, CodeText::VisualColumn(Text, (int32)Text.size(), TabSize));
        }
    }

    float FCodeEditor::LineTop(int32 Line) const
    {
        return Layout.WindowPos.y - Layout.ScrollY + RowOf(Line) * Layout.LineHeight;
    }

    ImVec2 FCodeEditor::ScreenAtPosition(FCodePosition Position) const
    {
        Position = Document.Clamp(Position);
        const int32 Visual = CodeText::VisualColumn(Document.GetLineView(Position.Line), Position.Column, Math::Max(1, Settings.TabSize));
        return ImVec2(Layout.WindowPos.x + Layout.TextOffset - Layout.ScrollX + Visual * Layout.CharWidth, LineTop(Position.Line));
    }

    FCodePosition FCodeEditor::PositionAtScreen(const ImVec2& Screen, bool bRoundToNearest) const
    {
        const int32 Line = LineAtRow((int32)std::floor((Screen.y - (Layout.WindowPos.y - Layout.ScrollY)) / Layout.LineHeight));
        const float Cells = (Screen.x - (Layout.WindowPos.x + Layout.TextOffset - Layout.ScrollX)) / Layout.CharWidth;
        const int32 Visual = Math::Max(0, (int32)std::floor(bRoundToNearest ? Cells + 0.5f : Cells));
        return { Line, CodeText::ColumnAtVisual(Document.GetLineView(Line), Visual, Math::Max(1, Settings.TabSize), bRoundToNearest) };
    }

    void FCodeEditor::HandleCompletionKeys(bool& bConsumed)
    {
        if (!Completion.bOpen || Completion.Visible.empty())
        {
            return;
        }
        const int32 Count = (int32)Completion.Visible.size();
        const CCodeEditorShortcuts& Keys = *GetDefault<CCodeEditorShortcuts>();
        bConsumed = true;
        if (Keys.MoveUp.IsPressed())
        {
            Completion.Selected = (Completion.Selected + Count - 1) % Count;
            Completion.bScrollToSelected = true;
        }
        else if (Keys.MoveDown.IsPressed())
        {
            Completion.Selected = (Completion.Selected + 1) % Count;
            Completion.bScrollToSelected = true;
        }
        else if (Keys.MovePageUp.IsPressed())
        {
            Completion.Selected = Math::Max(0, Completion.Selected - MaxCompletionRows);
            Completion.bScrollToSelected = true;
        }
        else if (Keys.MovePageDown.IsPressed())
        {
            Completion.Selected = Math::Min(Count - 1, Completion.Selected + MaxCompletionRows);
            Completion.bScrollToSelected = true;
        }
        else if (Keys.AcceptSuggestion.IsPressed())
        {
            AcceptCompletion();
        }
        else if (Keys.Dismiss.IsPressed())
        {
            CloseCompletion();
        }
        else
        {
            bConsumed = false;
        }
    }

    void FCodeEditor::HandleKeyboard()
    {
        if (!bFocused)
        {
            return;
        }
        ImGuiIO& Io = ImGui::GetIO();
        ImGui::SetNextFrameWantCaptureKeyboard(true);

        bool bConsumed = false;
        HandleCompletionKeys(bConsumed);
        if (bConsumed)
        {
            Io.InputQueueCharacters.resize(0);
            return;
        }

        const CCodeEditorShortcuts& Keys = *GetDefault<CCodeEditorShortcuts>();
        const FCodePosition Caret = Cursors[MainCursor].Head;

        struct FMoveBinding
        {
            const SShortcut& Shortcut;
            EMove            Move;
        };
        const FMoveBinding Moves[] =
        {
            { Keys.MoveLeft,            EMove::Left },
            { Keys.MoveRight,           EMove::Right },
            { Keys.MoveUp,              EMove::Up },
            { Keys.MoveDown,            EMove::Down },
            { Keys.MoveWordLeft,        EMove::WordLeft },
            { Keys.MoveWordRight,       EMove::WordRight },
            { Keys.MoveToLineStart,     EMove::LineStart },
            { Keys.MoveToLineEnd,       EMove::LineEnd },
            { Keys.MoveToDocumentStart, EMove::DocumentStart },
            { Keys.MoveToDocumentEnd,   EMove::DocumentEnd },
            { Keys.MovePageUp,          EMove::PageUp },
            { Keys.MovePageDown,        EMove::PageDown },
        };

        for (const FMoveBinding& Binding : Moves)
        {
            const bool bPlain = Binding.Shortcut.IsPressed();
            if (bPlain || Binding.Shortcut.IsPressed(ImGuiMod_Shift))
            {
                MoveCursors(Binding.Move, !bPlain);
                Io.InputQueueCharacters.resize(0);
                return;
            }
        }

        if (Keys.ScrollLineUp.IsPressed())                 { ImGui::SetScrollY(Layout.ScrollY - Layout.LineHeight); }
        else if (Keys.ScrollLineDown.IsPressed())          { ImGui::SetScrollY(Layout.ScrollY + Layout.LineHeight); }
        else if (Keys.MoveLinesUp.IsPressed())             { MoveLines(-1); }
        else if (Keys.MoveLinesDown.IsPressed())           { MoveLines(1); }
        else if (Keys.DuplicateLinesUp.IsPressed())        { DuplicateLines(-1); }
        else if (Keys.DuplicateLinesDown.IsPressed())      { DuplicateLines(1); }
        else if (Keys.AddCursorAbove.IsPressed())          { AddCursorVertical(-1); }
        else if (Keys.AddCursorBelow.IsPressed())          { AddCursorVertical(1); }
        else if (Keys.SelectAll.IsPressed())               { SelectAll(); }
        else if (Keys.Copy.IsPressed())                    { Copy(); }
        else if (Keys.Cut.IsPressed())                     { Cut(); }
        else if (Keys.Paste.IsPressed())                   { Paste(); }
        else if (Keys.Undo.IsPressed())                    { Undo(); }
        else if (Keys.Redo.IsPressed())                    { Redo(); }
        else if (Keys.DeleteLeft.IsPressed())              { Backspace(false); }
        else if (Keys.DeleteWordLeft.IsPressed())          { Backspace(true); }
        else if (Keys.DeleteRight.IsPressed())             { Delete(false); }
        else if (Keys.DeleteWordRight.IsPressed())         { Delete(true); }
        else if (Keys.NewLine.IsPressed())                 { NewLine(); }
        else if (Keys.InsertLineBelow.IsPressed())         { InsertLine(false); }
        else if (Keys.InsertLineAbove.IsPressed())         { InsertLine(true); }
        else if (Keys.InsertTab.IsPressed())
        {
            if (!AdvanceSnippet(1))
            {
                InsertTab();
            }
        }
        else if (Keys.TabBack.IsPressed())
        {
            if (!AdvanceSnippet(-1))
            {
                Outdent();
            }
        }
        else if (Keys.Indent.IsPressed())                  { Indent(); }
        else if (Keys.Outdent.IsPressed())                 { Outdent(); }
        else if (Keys.ToggleLineComment.IsPressed())       { ToggleLineComment(); }
        else if (Keys.ToggleBlockComment.IsPressed())      { ToggleBlockComment(); }
        else if (Keys.DeleteLines.IsPressed())             { DeleteLines(); }
        else if (Keys.AddNextOccurrence.IsPressed())       { AddNextOccurrence(); }
        else if (Keys.SelectAllOccurrences.IsPressed())    { SelectAllOccurrences(); }
        else if (Keys.Find.IsPressed())                    { OpenFind(false); }
        else if (Keys.Replace.IsPressed())                 { OpenFind(true); }
        else if (Keys.FindNext.IsPressed())                { FindNext(false); }
        else if (Keys.FindPrevious.IsPressed())            { FindNext(true); }
        else if (Keys.TriggerSuggestions.IsPressed())      { TriggerCompletion(); }
        else if (Keys.ParameterHints.IsPressed())          { TriggerSignatureHelp(); }
        else if (Keys.JumpToMatchingBracket.IsPressed())   { JumpToMatchingBracket(); }
        else if (Keys.GoToDefinition.IsPressed())          { GoToDefinition(Caret); }
        else if (Keys.FindReferences.IsPressed() && CommandHandler) { CommandHandler(ECommand::FindReferences, Caret); }
        else if (Keys.RenameSymbol.IsPressed() && CommandHandler)   { CommandHandler(ECommand::Rename, Caret); }
        else if (Keys.QuickFix.IsPressed())                { ShowCodeActions(); }
        else if (Keys.FormatDocument.IsPressed())          { FormatDocument(); }
        else if (Keys.ToggleFold.IsPressed())              { ToggleFold(Caret.Line); }
        else if (Keys.Unfold.IsPressed())                  { RevealLine(Caret.Line + 1); }
        else if (Keys.Dismiss.IsPressed())
        {
            if (bSignatureOpen)
            {
                bSignatureOpen = false;
            }
            else if (SnippetCurrent >= 0)
            {
                SnippetStops.clear();
                SnippetCurrent = -1;
            }
            else if (Cursors.size() > 1)
            {
                ClearExtraCursors();
            }
            else if (Find.bOpen)
            {
                Find.bOpen = false;
            }
        }

        if (!Io.InputQueueCharacters.empty())
        {
            if (!(Io.KeyCtrl && !Io.KeyAlt))
            {
                for (const ImWchar Character : Io.InputQueueCharacters)
                {
                    if (Character >= 32 && Character != 127)
                    {
                        TypeCharacter(Character);
                    }
                }
            }
            Io.InputQueueCharacters.resize(0);
        }
    }

    bool FCodeEditor::PickColorAt(FCodePosition Position)
    {
        if (Language == nullptr || Settings.bReadOnly)
        {
            return false;
        }
        TVector<FCodeColorSpan> Spans;
        Document.Highlight(*Language, Position.Line);
        const TVector<ECodeToken>& Tokens = Document.GetTokens(Position.Line);
        if (Tokens.size() != Document.GetLine(Position.Line).size())
        {
            return false;
        }
        Language->GetColorSpans(Document.GetLineView(Position.Line), Tokens.data(), Spans);
        for (const FCodeColorSpan& Span : Spans)
        {
            if (Span.Start <= Position.Column && Position.Column <= Span.End)
            {
                ColorPick.Range = { { Position.Line, Span.Start }, { Position.Line, Span.End } };
                const ImVec4 Color = ImGui::ColorConvertU32ToFloat4(Span.Color);
                ColorPick.Color[0] = Color.x;
                ColorPick.Color[1] = Color.y;
                ColorPick.Color[2] = Color.z;
                ColorPick.Color[3] = Color.w;
                ColorPick.bRequestOpen = true;
                return true;
            }
        }
        return false;
    }

    void FCodeEditor::HandleMouse()
    {
        ImGuiIO& Io = ImGui::GetIO();
        const ImVec2 Mouse = Io.MousePos;
        const bool bHovered = ImGui::IsWindowHovered() && Mouse.x >= Layout.ViewMin.x && Mouse.y >= Layout.ViewMin.y && Mouse.x < Layout.ViewMax.x && Mouse.y < Layout.ViewMax.y;
        const bool bOverGutter = Mouse.x < Layout.ViewMin.x + Layout.GutterWidth;
        const bool bOverMiniMap = Layout.MiniMapWidth > 0.0f && Mouse.x >= Layout.ViewMax.x - Layout.MiniMapWidth;

        if (bHovered)
        {
            if (Io.KeyCtrl && Io.MouseWheel != 0.0f)
            {
                Settings.FontScale = Math::Clamp(Settings.FontScale * (1.0f + Io.MouseWheel * 0.1f), 0.5f, 4.0f);
            }
            else if (Io.KeyShift && Io.MouseWheel != 0.0f)
            {
                ImGui::SetScrollX(Layout.ScrollX - Io.MouseWheel * Layout.CharWidth * 8.0f);
            }
            else if (Io.MouseWheel != 0.0f || Io.MouseWheelH != 0.0f)
            {
                ImGui::SetScrollY(Layout.ScrollY - Io.MouseWheel * Layout.LineHeight * 3.0f);
                ImGui::SetScrollX(Layout.ScrollX - Io.MouseWheelH * Layout.CharWidth * 8.0f);
            }
            if (!bOverGutter && !bOverMiniMap)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
            }
        }

        auto ScrollMiniMapTo = [this](float MouseY)
        {
            const int32 Line = Layout.MiniMapFirstLine + (int32)((MouseY - Layout.ViewMin.y) / Layout.MiniMapRowHeight);
            ImGui::SetScrollY(Math::Max(0.0f, RowOf(Line) * Layout.LineHeight - (Layout.ViewMax.y - Layout.ViewMin.y) * 0.5f));
        };

        if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            CloseCompletion();
            HoverState.bValid = false;
            const FCodePosition Position = Document.Clamp(PositionAtScreen(Mouse, true));
            if (bOverMiniMap)
            {
                bDraggingMiniMap = true;
                ScrollMiniMapTo(Mouse.y);
            }
            else if (bOverGutter && Mouse.x >= Layout.ViewMin.x + Layout.GutterWidth - Layout.CharWidth * 1.5f && FoldStartingAt(Position.Line) != nullptr)
            {
                ToggleFold(Position.Line);
            }
            else if (bOverGutter)
            {
                const int32 Line = Position.Line;
                const FCodePosition LineEnd = Line + 1 < Document.GetLineCount() ? FCodePosition{ Line + 1, 0 } : FCodePosition{ Line, (int32)Document.GetLine(Line).size() };
                ClearExtraCursors();
                if (Io.KeyShift)
                {
                    Cursors[0].Head = LineEnd;
                }
                else
                {
                    Cursors[0].Anchor = { Line, 0 };
                    Cursors[0].Head = LineEnd;
                }
                MouseSelectOrigin = { { Line, 0 }, LineEnd };
                bSelectingLines = true;
                OnCursorsMoved();
            }
            else if (!(Io.KeyCtrl && !Io.KeyAlt && (PickColorAt(Position) || GoToDefinition(Position))))
            {
                const int32 Clicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
                if (Io.KeyAlt && !Io.KeyCtrl)
                {
                    FCursor& Added = Cursors.emplace_back();
                    Added.Anchor = Position;
                    Added.Head = Position;
                    MainCursor = (int32)Cursors.size() - 1;
                    NormalizeCursors();
                }
                else if (Io.KeyShift)
                {
                    ClearExtraCursors();
                    Cursors[0].Head = Position;
                    MouseSelectOrigin = { Cursors[0].Anchor, Cursors[0].Anchor };
                    bSelectingWithMouse = true;
                }
                else if (Clicks == 2)
                {
                    ClearExtraCursors();
                    MouseSelectOrigin = WordAt(Position);
                    Cursors[0].Anchor = MouseSelectOrigin.Start;
                    Cursors[0].Head = MouseSelectOrigin.End;
                    bSelectingWords = true;
                }
                else if (Clicks >= 3)
                {
                    ClearExtraCursors();
                    const FCodePosition LineEnd = Position.Line + 1 < Document.GetLineCount() ? FCodePosition{ Position.Line + 1, 0 } : FCodePosition{ Position.Line, (int32)Document.GetLine(Position.Line).size() };
                    MouseSelectOrigin = { { Position.Line, 0 }, LineEnd };
                    Cursors[0].Anchor = MouseSelectOrigin.Start;
                    Cursors[0].Head = MouseSelectOrigin.End;
                    bSelectingLines = true;
                }
                else
                {
                    ClearExtraCursors();
                    Cursors[0].Anchor = Position;
                    Cursors[0].Head = Position;
                    MouseSelectOrigin = { Position, Position };
                    bSelectingWithMouse = true;
                }
                Cursors[MainCursor].PreferredVisual = -1;
                OnCursorsMoved();
            }
        }

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && bDraggingMiniMap)
        {
            ScrollMiniMapTo(Mouse.y);
        }
        else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (bSelectingWithMouse || bSelectingWords || bSelectingLines))
        {
            if (Mouse.y < Layout.ViewMin.y)
            {
                ImGui::SetScrollY(Layout.ScrollY - Layout.LineHeight);
            }
            else if (Mouse.y > Layout.ViewMax.y)
            {
                ImGui::SetScrollY(Layout.ScrollY + Layout.LineHeight);
            }
            if (Mouse.x < Layout.ViewMin.x + Layout.TextOffset)
            {
                ImGui::SetScrollX(Layout.ScrollX - Layout.CharWidth * 2.0f);
            }
            else if (Mouse.x > Layout.ViewMax.x - Layout.MiniMapWidth)
            {
                ImGui::SetScrollX(Layout.ScrollX + Layout.CharWidth * 2.0f);
            }

            const FCodePosition Position = Document.Clamp(PositionAtScreen(Mouse, true));
            FCursor& Main = Cursors[MainCursor];
            if (bSelectingWords || bSelectingLines)
            {
                FCodeRange Extent = MouseSelectOrigin;
                if (bSelectingWords)
                {
                    const FCodeRange Word = WordAt(Position);
                    Extent = Position < MouseSelectOrigin.Start ? FCodeRange{ Word.Start, MouseSelectOrigin.End } : FCodeRange{ MouseSelectOrigin.Start, Word.End < MouseSelectOrigin.End ? MouseSelectOrigin.End : Word.End };
                    Main.Anchor = Position < MouseSelectOrigin.Start ? MouseSelectOrigin.End : MouseSelectOrigin.Start;
                    Main.Head = Position < MouseSelectOrigin.Start ? Extent.Start : Extent.End;
                }
                else
                {
                    const FCodePosition LineEnd = Position.Line + 1 < Document.GetLineCount() ? FCodePosition{ Position.Line + 1, 0 } : FCodePosition{ Position.Line, (int32)Document.GetLine(Position.Line).size() };
                    const bool bAbove = Position.Line < MouseSelectOrigin.Start.Line;
                    Main.Anchor = bAbove ? MouseSelectOrigin.End : MouseSelectOrigin.Start;
                    Main.Head = bAbove ? FCodePosition{ Position.Line, 0 } : LineEnd;
                }
            }
            else
            {
                Main.Head = Position;
            }
            CursorBlinkStart = Now();
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (bSelectingWithMouse || bSelectingWords || bSelectingLines)
            {
                NormalizeCursors();
                OnCursorsMoved();
            }
            bSelectingWithMouse = false;
            bSelectingWords = false;
            bSelectingLines = false;
            bDraggingMiniMap = false;
        }

        if (bHovered && !bOverGutter && !bOverMiniMap && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            const FCodePosition Position = Document.Clamp(PositionAtScreen(Mouse, true));
            if (!Cursors[MainCursor].GetRange().Contains(Position))
            {
                SetCursor(Position, false);
            }
            ContextMenuPosition = Position;
            ImGui::OpenPopup("##CodeEditorContext");
        }

        if (!bHovered || bOverGutter || bOverMiniMap || ImGui::IsAnyMouseDown())
        {
            HoverState.bValid = false;
            HoverState.bQueried = false;
            HoverState.StillSince = Now();
            HoverState.MousePos = Mouse;
            return;
        }
        const float Moved = std::fabs(Mouse.x - HoverState.MousePos.x) + std::fabs(Mouse.y - HoverState.MousePos.y);
        if (Moved > 3.0f)
        {
            HoverState.MousePos = Mouse;
            HoverState.StillSince = Now();
            HoverState.bQueried = false;
            HoverState.bValid = false;
            return;
        }
        if (HoverState.bQueried || Now() - HoverState.StillSince < HoverDelay)
        {
            return;
        }

        HoverState.bQueried = true;
        HoverState.bValid = false;
        HoverState.Hover = {};
        HoverState.DiagnosticText.clear();
        const FCodePosition Position = Document.Clamp(PositionAtScreen(Mouse, false));
        const FStringView Line = Document.GetLineView(Position.Line);
        if (Position.Column >= (int32)Line.size())
        {
            return;
        }

        auto Consider = [&](const TVector<FCodeDiagnostic>& Diagnostics)
        {
            for (const FCodeDiagnostic& Diagnostic : Diagnostics)
            {
                FCodeRange Range = Diagnostic.Range;
                if (Range.Start.Line == Range.End.Line && Range.End.Column <= Range.Start.Column)
                {
                    Range.End.Column = Range.Start.Column + 1;
                }
                const bool bWholeLine = Range.Start.Column == 0 && Range.End.Line > Range.Start.Line;
                if (Range.Contains(Position) || (bWholeLine && Position.Line == Range.Start.Line))
                {
                    if (!HoverState.DiagnosticText.empty())
                    {
                        HoverState.DiagnosticText += '\n';
                    }
                    HoverState.DiagnosticText += Diagnostic.Message;
                    HoverState.DiagnosticColor = SeverityColor(Diagnostic.Severity);
                }
            }
        };
        Consider(LanguageDiagnostics);
        Consider(ExternalDiagnostics);

        const bool bHover = Language != nullptr && Language->GetHover(Document, Position, HoverState.Hover);
        HoverState.bValid = bHover || !HoverState.DiagnosticText.empty();
    }

    void FCodeEditor::DrawBackgrounds(ImDrawList* DrawList)
    {
        const float Left = Layout.ViewMin.x + Layout.GutterWidth;
        const float Right = Layout.ViewMax.x - Layout.MiniMapWidth;
        const float LineHeight = Layout.LineHeight;
        const int32 TabSize = Math::Max(1, Settings.TabSize);

        for (const FCursor& Cursor : Cursors)
        {
            if (!Cursor.HasSelection() && Cursor.Head.Line >= Layout.FirstLine && Cursor.Head.Line <= Layout.LastLine)
            {
                const float Top = LineTop(Cursor.Head.Line);
                DrawList->AddRectFilled(ImVec2(Left, Top), ImVec2(Right, Top + LineHeight), Palette.CurrentLine);
            }
        }

        if (Settings.bShowIndentGuides)
        {
            auto IndentOf = [this, TabSize](int32 Line) -> int32
            {
                const FStringView Text = Document.GetLineView(Line);
                return CodeText::IsBlank(Text) ? -1 : CodeText::VisualColumn(Text, CodeText::LeadingWhitespace(Text), TabSize);
            };
            for (int32 Line = Layout.FirstLine; Line <= Layout.LastLine; ++Line)
            {
                if (IsLineHidden(Line))
                {
                    continue;
                }
                int32 Indent = IndentOf(Line);
                if (Indent < 0)
                {
                    int32 Above = -1;
                    int32 Below = -1;
                    for (int32 Scan = Line - 1; Scan >= Math::Max(0, Line - 64) && Above < 0; --Scan)
                    {
                        Above = IndentOf(Scan);
                    }
                    for (int32 Scan = Line + 1; Scan < Math::Min(Document.GetLineCount(), Line + 64) && Below < 0; ++Scan)
                    {
                        Below = IndentOf(Scan);
                    }
                    Indent = Math::Max(0, Math::Min(Above, Below));
                }
                const float Top = LineTop(Line);
                for (int32 Level = TabSize; Level < Indent; Level += TabSize)
                {
                    const float X = std::floor(Layout.WindowPos.x + Layout.TextOffset - Layout.ScrollX + Level * Layout.CharWidth) + 0.5f;
                    DrawList->AddLine(ImVec2(X, Top), ImVec2(X, Top + LineHeight), Palette.IndentGuide);
                }
            }
        }

        auto FillRange = [&](const FCodeRange& Range, ImU32 Color, float Rounding)
        {
            const int32 First = Math::Max(Range.Start.Line, Layout.FirstLine);
            const int32 Last = Math::Min(Range.End.Line, Layout.LastLine);
            for (int32 Line = First; Line <= Last; ++Line)
            {
                if (IsLineHidden(Line))
                {
                    continue;
                }
                const int32 StartColumn = Line == Range.Start.Line ? Range.Start.Column : 0;
                const int32 EndColumn = Line == Range.End.Line ? Range.End.Column : (int32)Document.GetLine(Line).size();
                const float Top = LineTop(Line);
                const float X0 = ScreenAtPosition({ Line, StartColumn }).x;
                float X1 = ScreenAtPosition({ Line, EndColumn }).x;
                if (Line != Range.End.Line)
                {
                    X1 += Layout.CharWidth * 0.5f;
                }
                if (X1 > X0)
                {
                    DrawList->AddRectFilled(ImVec2(X0, Top), ImVec2(X1, Top + LineHeight), Color, Rounding);
                }
            }
        };

        if (Find.bOpen && !Find.Matches.empty())
        {
            for (const FCodeRange& Match : Find.Matches)
            {
                if (Match.Start.Line > Layout.LastLine)
                {
                    break;
                }
                if (Match.End.Line >= Layout.FirstLine)
                {
                    FillRange(Match, Palette.FindMatch, 2.0f);
                }
            }
        }

        if (Settings.bHighlightOccurrences && Cursors.size() == 1)
        {
            const FCursor& Main = Cursors[0];
            FString Needle;
            bool bWholeWord = false;
            if (Main.HasSelection())
            {
                const FCodeRange Range = Main.GetRange();
                if (Range.Start.Line == Range.End.Line && Range.End.Column - Range.Start.Column <= 200)
                {
                    Needle = Document.GetRangeText(Range);
                }
            }
            else
            {
                const FCodeRange Word = WordAt(Main.Head);
                const ECodeToken Token = Document.GetTokenAt(Word.Start);
                if (Word.End.Column - Word.Start.Column >= 2 && Token != ECodeToken::Comment && Token != ECodeToken::String)
                {
                    Needle = Document.GetRangeText(Word);
                    bWholeWord = true;
                }
            }
            if (!Needle.empty() && !CodeText::IsBlank(Needle.View()))
            {
                auto IsWord = [this](char C) { return Language != nullptr ? Language->IsWordChar(C) : (CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '_'); };
                for (int32 Line = Layout.FirstLine; Line <= Layout.LastLine; ++Line)
                {
                    if (IsLineHidden(Line))
                    {
                        continue;
                    }
                    const FStringView Text = Document.GetLineView(Line);
                    for (size_t Found = Text.find(Needle.View()); Found != FStringView::npos; Found = Text.find(Needle.View(), Found + 1))
                    {
                        const size_t End = Found + Needle.size();
                        if (bWholeWord && ((Found > 0 && IsWord(Text[Found - 1])) || (End < Text.size() && IsWord(Text[End]))))
                        {
                            continue;
                        }
                        FillRange({ { Line, (int32)Found }, { Line, (int32)End } }, Palette.WordHighlight, 2.0f);
                    }
                }
            }
        }

        for (int32 i = SnippetCurrent + 1; SnippetCurrent >= 0 && i < (int32)SnippetStops.size(); ++i)
        {
            const FCodeRange& Range = SnippetStops[i].Range;
            if (Range.Start.Line >= Layout.FirstLine && Range.Start.Line <= Layout.LastLine && !Range.IsEmpty())
            {
                const ImVec2 Min = ScreenAtPosition(Range.Start);
                const ImVec2 Max(ScreenAtPosition(Range.End).x, Min.y + LineHeight);
                DrawList->AddRect(Min, Max, Palette.WordHighlight | IM_COL32_A_MASK, 2.0f);
            }
        }

        for (const FCursor& Cursor : Cursors)
        {
            if (Cursor.HasSelection())
            {
                FillRange(Cursor.GetRange(), bFocused ? Palette.Selection : EditorColors::ScaleAlpha(Palette.Selection, 0.6f), 3.0f);
            }
        }

        if (Settings.bMatchBrackets && Cursors.size() == 1 && !Cursors[0].HasSelection())
        {
            FCodePosition Open;
            FCodePosition Close;
            if (FindBracketPair(Cursors[0].Head, Open, Close))
            {
                for (const FCodePosition Bracket : { Open, Close })
                {
                    if (Bracket.Line < Layout.FirstLine || Bracket.Line > Layout.LastLine)
                    {
                        continue;
                    }
                    const ImVec2 Min = ScreenAtPosition(Bracket);
                    const ImVec2 Max(Min.x + Layout.CharWidth, Min.y + LineHeight);
                    DrawList->AddRectFilled(Min, Max, EditorColors::ScaleAlpha(Palette.BracketMatch, 0.45f), 2.0f);
                    DrawList->AddRect(Min, Max, Palette.BracketMatch, 2.0f);
                }
            }
        }
    }

    void FCodeEditor::DrawTextRuns(ImDrawList* DrawList)
    {
        const int32 TabSize = Math::Max(1, Settings.TabSize);
        const float ClipLeft = Layout.ViewMin.x + Layout.GutterWidth;
        const float ClipRight = Layout.ViewMax.x - Layout.MiniMapWidth;
        const float BaseX = Layout.WindowPos.x + Layout.TextOffset - Layout.ScrollX;
        const float TextOffsetY = (Layout.LineHeight - Layout.FontSize) * 0.5f;
        const ImU32 DefaultColor = Palette[ECodeToken::Default];
        TVector<FCodeColorSpan> ColorSpans;

        for (int32 Line = Layout.FirstLine; Line <= Layout.LastLine; ++Line)
        {
            if (IsLineHidden(Line))
            {
                continue;
            }
            const FStringView Text = Document.GetLineView(Line);
            const TVector<ECodeToken>& Tokens = Document.GetTokens(Line);
            const bool bHasTokens = Language != nullptr && Tokens.size() == Text.size();
            const float Top = LineTop(Line);
            const float Y = Top + TextOffsetY;
            const int32 Size = (int32)Text.size();

            // A collapsed block shows as its first line, an ellipsis and its closing line when that is only a brace.
            if (IsCollapsed(Line))
            {
                if (const FCodeFold* Fold = FoldStartingAt(Line))
                {
                    const float EndX = BaseX + (CodeText::VisualColumn(Text, Size, TabSize) + 1) * Layout.CharWidth;
                    const ImVec2 Min(EndX, Top + 2.0f);
                    const ImVec2 Max(EndX + Layout.CharWidth * 3.0f, Top + Layout.LineHeight - 2.0f);
                    DrawList->AddRectFilled(Min, Max, EditorColors::ScaleAlpha(Palette.WordHighlight, 1.4f), 3.0f);
                    DrawList->AddText(Layout.Font, Layout.FontSize, ImVec2(Min.x + Layout.CharWidth * 0.5f, Y), Palette[ECodeToken::Comment], "...");
                    const FStringView Closing = Document.GetLineView(Math::Min(Fold->EndLine, Document.GetLineCount() - 1));
                    const FStringView Trimmed = Closing.substr((size_t)CodeText::LeadingWhitespace(Closing));
                    if (!Trimmed.empty() && Trimmed.size() <= 3 && Trimmed.front() == '}')
                    {
                        DrawList->AddText(Layout.Font, Layout.FontSize, ImVec2(Max.x + Layout.CharWidth, Y), Palette[ECodeToken::Punctuation], Trimmed.data(), Trimmed.data() + Trimmed.size());
                    }
                }
            }

            int32 Visual = 0;
            int32 i = 0;
            while (i < Size)
            {
                const char C = Text[i];
                const float CellX = BaseX + Visual * Layout.CharWidth;
                if (CellX > ClipRight)
                {
                    break;
                }
                if (C == '\t')
                {
                    const int32 Width = TabSize - Visual % TabSize;
                    if (Settings.bShowWhitespace && CellX + Width * Layout.CharWidth >= ClipLeft)
                    {
                        const float MidY = Top + Layout.LineHeight * 0.5f;
                        const float EndX = CellX + Width * Layout.CharWidth - Layout.CharWidth * 0.4f;
                        DrawList->AddLine(ImVec2(CellX + Layout.CharWidth * 0.3f, MidY), ImVec2(EndX, MidY), Palette.Whitespace);
                        DrawList->AddLine(ImVec2(EndX - 3.0f, MidY - 3.0f), ImVec2(EndX, MidY), Palette.Whitespace);
                        DrawList->AddLine(ImVec2(EndX - 3.0f, MidY + 3.0f), ImVec2(EndX, MidY), Palette.Whitespace);
                    }
                    Visual += Width;
                    ++i;
                    continue;
                }
                if (C == ' ')
                {
                    if (Settings.bShowWhitespace && CellX >= ClipLeft - Layout.CharWidth)
                    {
                        DrawList->AddCircleFilled(ImVec2(CellX + Layout.CharWidth * 0.5f, Top + Layout.LineHeight * 0.5f), 1.2f, Palette.Whitespace);
                    }
                    ++Visual;
                    ++i;
                    continue;
                }

                const int32 RunStart = i;
                const int32 RunVisual = Visual;
                const ECodeToken Token = bHasTokens ? Tokens[i] : ECodeToken::Default;
                if ((uint8)C >= 0x80)
                {
                    i = CodeText::NextCharStart(Text, i);
                    ++Visual;
                }
                else
                {
                    while (i < Size && Text[i] != ' ' && Text[i] != '\t' && (uint8)Text[i] < 0x80 && (!bHasTokens || Tokens[i] == Token))
                    {
                        ++i;
                        ++Visual;
                    }
                }
                const float RunX = BaseX + RunVisual * Layout.CharWidth;
                if (RunX + (Visual - RunVisual) * Layout.CharWidth >= ClipLeft)
                {
                    const ImU32 Color = bHasTokens ? Palette[Token] : DefaultColor;
                    DrawList->AddText(Layout.Font, Layout.FontSize, ImVec2(RunX, Y), Color, Text.data() + RunStart, Text.data() + i);
                }
            }

            if (bHasTokens)
            {
                ColorSpans.clear();
                Language->GetColorSpans(Text, Tokens.data(), ColorSpans);
                for (const FCodeColorSpan& Span : ColorSpans)
                {
                    const float X0 = ScreenAtPosition({ Line, Span.Start }).x;
                    const float X1 = ScreenAtPosition({ Line, Span.End }).x;
                    const float Bar = Math::Max(2.0f, std::floor(Layout.LineHeight * 0.1f));
                    DrawList->AddRectFilled(ImVec2(X0, Top + Layout.LineHeight - Bar - 1.0f), ImVec2(X1, Top + Layout.LineHeight - 1.0f), Span.Color | IM_COL32_A_MASK, Bar * 0.5f);
                }
            }
        }
    }

    ImU32 FCodeEditor::SeverityColor(ECodeSeverity Severity) const
    {
        return Severity == ECodeSeverity::Error ? Palette.Error : Severity == ECodeSeverity::Warning ? Palette.Warning : Palette.Info;
    }

    void FCodeEditor::DrawSquiggle(ImDrawList* DrawList, float X0, float X1, float Y, ImU32 Color) const
    {
        constexpr float Step = 2.5f;
        constexpr float Amplitude = 1.5f;
        ImVec2 Points[256];
        int32 Count = 0;
        for (float X = X0; X <= X1 && Count < 256; X += Step)
        {
            Points[Count] = ImVec2(X, Y + ((Count & 1) ? -Amplitude : Amplitude));
            ++Count;
        }
        if (Count >= 2)
        {
            DrawList->AddPolyline(Points, Count, Color, 1.0f);
        }
    }

    void FCodeEditor::DrawDiagnostics(ImDrawList* DrawList)
    {
        auto DrawSet = [&](const TVector<FCodeDiagnostic>& Diagnostics)
        {
            for (const FCodeDiagnostic& Diagnostic : Diagnostics)
            {
                const FCodeRange Range{ Document.Clamp(Diagnostic.Range.Start), Document.Clamp(Diagnostic.Range.End) };
                const int32 First = Math::Max(Range.Start.Line, Layout.FirstLine);
                const int32 Last = Math::Min(Math::Max(Range.Start.Line, Range.End.Line - (Range.End.Column == 0 ? 1 : 0)), Layout.LastLine);
                for (int32 Line = First; Line <= Last; ++Line)
                {
                    if (IsLineHidden(Line))
                    {
                        continue;
                    }
                    const FStringView Text = Document.GetLineView(Line);
                    int32 StartColumn = Line == Range.Start.Line ? Range.Start.Column : CodeText::LeadingWhitespace(Text);
                    int32 EndColumn = Line == Range.End.Line ? Range.End.Column : (int32)Text.size();
                    if (StartColumn == 0 && EndColumn == (int32)Text.size())
                    {
                        StartColumn = CodeText::LeadingWhitespace(Text);
                    }
                    const float X0 = ScreenAtPosition({ Line, StartColumn }).x;
                    float X1 = ScreenAtPosition({ Line, EndColumn }).x;
                    X1 = Math::Max(X1, X0 + Layout.CharWidth);
                    DrawSquiggle(DrawList, X0, X1, LineTop(Line) + Layout.LineHeight - 2.5f, SeverityColor(Diagnostic.Severity));
                }
            }
        };
        DrawSet(LanguageDiagnostics);
        DrawSet(ExternalDiagnostics);
    }

    void FCodeEditor::DrawCursors(ImDrawList* DrawList)
    {
        if (!bFocused || std::fmod(Now() - CursorBlinkStart, (double)CursorBlinkPeriod) > CursorBlinkPeriod * 0.55)
        {
            return;
        }
        const float Width = Math::Max(2.0f, std::floor(Layout.FontSize * 0.11f));
        for (const FCursor& Cursor : Cursors)
        {
            if (Cursor.Head.Line < Layout.FirstLine || Cursor.Head.Line > Layout.LastLine || IsLineHidden(Cursor.Head.Line))
            {
                continue;
            }
            const ImVec2 At = ScreenAtPosition(Cursor.Head);
            DrawList->AddRectFilled(ImVec2(At.x - Width * 0.5f, At.y + 1.0f), ImVec2(At.x + Width * 0.5f, At.y + Layout.LineHeight - 1.0f), Palette.Cursor);
        }
    }

    void FCodeEditor::DrawGutter(ImDrawList* DrawList)
    {
        const ImVec2 Min = Layout.ViewMin;
        const ImVec2 Max(Layout.ViewMin.x + Layout.GutterWidth, Layout.ViewMax.y);
        DrawList->PushClipRect(Min, Max, true);
        DrawList->AddRectFilled(Min, Max, Palette.Gutter);

        const int32 Visible = Layout.LastLine - Layout.FirstLine + 1;
        TVector<int8> Severity;
        Severity.resize(Math::Max(0, Visible), (int8)-1);
        auto Mark = [&](const TVector<FCodeDiagnostic>& Diagnostics)
        {
            for (const FCodeDiagnostic& Diagnostic : Diagnostics)
            {
                const int32 Line = Diagnostic.Range.Start.Line;
                if (Line >= Layout.FirstLine && Line <= Layout.LastLine)
                {
                    int8& Slot = Severity[Line - Layout.FirstLine];
                    const int8 Rank = (int8)(2 - (int32)Diagnostic.Severity);
                    Slot = Math::Max(Slot, Rank);
                }
            }
        };
        Mark(LanguageDiagnostics);
        Mark(ExternalDiagnostics);

        const float TextOffsetY = (Layout.LineHeight - Layout.FontSize) * 0.5f;
        const float FoldColumn = Max.x - Layout.CharWidth * 1.5f;
        const bool bGutterHovered = ImGui::IsWindowHovered() && ImGui::GetIO().MousePos.x < Max.x;
        for (int32 Line = Layout.FirstLine; Line <= Layout.LastLine; ++Line)
        {
            if (IsLineHidden(Line))
            {
                continue;
            }
            const float Top = LineTop(Line);
            const bool bActive = Algo::AnyOf(Cursors.begin(), Cursors.end(), [Line](const FCursor& Cursor) { return Cursor.Head.Line == Line; });
            if (FoldStartingAt(Line) != nullptr && (bGutterHovered || IsCollapsed(Line)))
            {
                const ImVec2 Center(FoldColumn + Layout.CharWidth * 0.7f, Top + Layout.LineHeight * 0.5f);
                const float Size = Math::Max(3.0f, Layout.FontSize * 0.22f);
                const ImU32 Color = IsCollapsed(Line) ? Palette.LineNumberActive : Palette.LineNumber;
                if (IsCollapsed(Line))
                {
                    DrawList->AddTriangleFilled(ImVec2(Center.x - Size * 0.6f, Center.y - Size), ImVec2(Center.x - Size * 0.6f, Center.y + Size), ImVec2(Center.x + Size * 0.8f, Center.y), Color);
                }
                else
                {
                    DrawList->AddTriangleFilled(ImVec2(Center.x - Size, Center.y - Size * 0.6f), ImVec2(Center.x + Size, Center.y - Size * 0.6f), ImVec2(Center.x, Center.y + Size * 0.8f), Color);
                }
            }
            if (Settings.bShowLineNumbers)
            {
                char Number[16];
                const int32 Length = std::snprintf(Number, sizeof(Number), "%d", Line + 1);
                const float X = FoldColumn - Layout.CharWidth * 0.25f - Length * Layout.CharWidth;
                DrawList->AddText(Layout.Font, Layout.FontSize, ImVec2(X, Top + TextOffsetY), bActive ? Palette.LineNumberActive : Palette.LineNumber, Number, Number + Length);
            }
            const int8 Rank = Severity[Line - Layout.FirstLine];
            if (Rank >= 0)
            {
                const ImU32 Color = SeverityColor((ECodeSeverity)(2 - Rank));
                DrawList->AddRectFilled(ImVec2(Min.x + 2.0f, Top + 3.0f), ImVec2(Min.x + 5.0f, Top + Layout.LineHeight - 3.0f), Color, 1.5f);
                // A problem on the caret's line may have a fix, so the line shows a bulb that Ctrl+. or a click opens.
                if (bActive && Rank >= 1 && Language != nullptr && !Settings.bReadOnly)
                {
                    const ImVec2 Bulb(Min.x + Layout.CharWidth * 1.1f, Top + Layout.LineHeight * 0.42f);
                    const float Radius = Math::Max(3.0f, Layout.FontSize * 0.24f);
                    DrawList->AddCircleFilled(Bulb, Radius, Palette.Warning);
                    DrawList->AddRectFilled(ImVec2(Bulb.x - Radius * 0.45f, Bulb.y + Radius * 0.7f), ImVec2(Bulb.x + Radius * 0.45f, Bulb.y + Radius * 1.5f), Palette.LineNumber);
                    const ImVec2 Mouse = ImGui::GetIO().MousePos;
                    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && Math::Abs(Mouse.x - Bulb.x) <= Radius * 1.6f && Math::Abs(Mouse.y - Bulb.y) <= Radius * 1.8f)
                    {
                        ShowCodeActions();
                    }
                }
            }
        }
        DrawList->PopClipRect();
    }

    void FCodeEditor::DrawMiniMap(ImDrawList* DrawList)
    {
        if (Layout.MiniMapWidth <= 0.0f)
        {
            return;
        }
        const ImVec2 Min(Layout.ViewMax.x - Layout.MiniMapWidth, Layout.ViewMin.y);
        const ImVec2 Max = Layout.ViewMax;
        DrawList->PushClipRect(Min, Max, true);
        DrawList->AddRectFilled(Min, Max, EditorColors::ScaleAlpha(Palette.Gutter, 0.6f));
        DrawList->AddLine(ImVec2(Min.x, Min.y), ImVec2(Min.x, Max.y), Palette.IndentGuide);

        const float RowHeight = Layout.MiniMapRowHeight;
        const int32 Capacity = Math::Max(1, (int32)((Max.y - Min.y) / RowHeight));
        const int32 Total = Document.GetLineCount();
        const float MaxScroll = Math::Max(1.0f, Layout.ContentHeight - (Layout.ViewMax.y - Layout.ViewMin.y));
        const int32 First = Total <= Capacity ? 0 : (int32)(Math::Clamp(Layout.ScrollY / MaxScroll, 0.0f, 1.0f) * (Total - Capacity));
        const int32 Last = Math::Min(Total - 1, First + Capacity);
        Layout.MiniMapFirstLine = First;
        if (Language != nullptr)
        {
            Document.Highlight(*Language, Last);
        }

        const float CellWidth = Math::Clamp(Layout.MiniMapWidth / 90.0f, 0.6f, 1.2f);
        const float Left = Min.x + 5.0f;
        const int32 TabSize = Math::Max(1, Settings.TabSize);
        for (int32 Line = First; Line <= Last; ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            const TVector<ECodeToken>& Tokens = Document.GetTokens(Line);
            const bool bHasTokens = Language != nullptr && Tokens.size() == Text.size();
            const float Y = Min.y + (Line - First) * RowHeight;
            int32 Visual = 0;
            int32 i = 0;
            while (i < (int32)Text.size() && Left + Visual * CellWidth < Max.x)
            {
                if (Text[i] == ' ' || Text[i] == '\t')
                {
                    Visual += Text[i] == '\t' ? TabSize - Visual % TabSize : 1;
                    ++i;
                    continue;
                }
                const int32 RunVisual = Visual;
                const ECodeToken Token = bHasTokens ? Tokens[i] : ECodeToken::Default;
                while (i < (int32)Text.size() && Text[i] != ' ' && Text[i] != '\t' && (!bHasTokens || Tokens[i] == Token))
                {
                    i = CodeText::NextCharStart(Text, i);
                    ++Visual;
                }
                DrawList->AddRectFilled(ImVec2(Left + RunVisual * CellWidth, Y), ImVec2(Left + Visual * CellWidth, Y + RowHeight - 0.5f), EditorColors::ScaleAlpha(Palette[Token], 0.55f));
            }
        }

        const float ViewTop = Min.y + (Layout.FirstLine - First) * RowHeight;
        const float ViewBottom = Min.y + (Layout.LastLine + 1 - First) * RowHeight;
        DrawList->AddRectFilled(ImVec2(Min.x, ViewTop), ImVec2(Max.x, ViewBottom), Palette.MiniMapViewport);
        DrawList->PopClipRect();
    }

    void FCodeEditor::DrawCompletionPopup()
    {
        bCompletionHovered = false;
        if (!Completion.bOpen || Completion.Visible.empty())
        {
            return;
        }

        const int32 Count = (int32)Completion.Visible.size();
        const int32 Rows = Math::Min(MaxCompletionRows, Count);
        const float RowHeight = std::floor(Layout.FontSize * 1.55f);
        const float IconWidth = Layout.FontSize * 1.6f;
        const ImVec2 Padding(4.0f, 4.0f);

        float LabelWidth = 0.0f;
        float DetailWidth = 0.0f;
        for (int32 i = 0; i < Math::Min(Count, 200); ++i)
        {
            const FCodeCompletionItem& Item = Completion.Items[Completion.Visible[i].Item];
            LabelWidth = Math::Max(LabelWidth, Item.Label.size() * Layout.CharWidth);
            DetailWidth = Math::Max(DetailWidth, Math::Min(Item.Detail.size(), (size_t)40) * Layout.CharWidth);
        }
        const float Width = Math::Clamp(IconWidth + LabelWidth + DetailWidth + Layout.CharWidth * 4.0f + Padding.x * 2.0f, 260.0f, 640.0f);
        const float Height = Rows * RowHeight + Padding.y * 2.0f;

        const ImVec2 Anchor = ScreenAtPosition(Completion.WordStart);
        ImVec2 Position(Anchor.x - IconWidth - Padding.x, Anchor.y + Layout.LineHeight + 2.0f);
        const ImGuiViewport* Viewport = ImGui::GetWindowViewport();
        if (Position.y + Height > Viewport->WorkPos.y + Viewport->WorkSize.y)
        {
            Position.y = Anchor.y - Height - 2.0f;
        }
        Position.x = Math::Min(Position.x, Viewport->WorkPos.x + Viewport->WorkSize.x - Width - 4.0f);

        ImGui::SetNextWindowPos(Position);
        ImGui::SetNextWindowSize(ImVec2(Width, Height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Padding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Palette.PopupBackground);
        ImGui::PushStyleColor(ImGuiCol_Border, Palette.PopupBorder);
        constexpr ImGuiWindowFlags Flags = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking;

        int32 Clicked = -1;
        ImVec2 ListMin;
        ImVec2 ListMax;
        if (ImGui::Begin("##CodeEditorCompletion", nullptr, Flags))
        {
            bCompletionHovered = ImGui::IsWindowHovered();
            if (bCompletionHovered && ImGui::GetIO().MouseWheel != 0.0f)
            {
                Completion.FirstRow -= (int32)(ImGui::GetIO().MouseWheel * 3.0f);
            }
            else if (Completion.bScrollToSelected)
            {
                if (Completion.Selected < Completion.FirstRow)
                {
                    Completion.FirstRow = Completion.Selected;
                }
                else if (Completion.Selected >= Completion.FirstRow + Rows)
                {
                    Completion.FirstRow = Completion.Selected - Rows + 1;
                }
            }
            Completion.bScrollToSelected = false;
            Completion.FirstRow = Math::Clamp(Completion.FirstRow, 0, Count - Rows);

            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            const ImVec2 Origin = ImGui::GetCursorScreenPos();
            const float Inner = Width - Padding.x * 2.0f;
            ListMin = ImGui::GetWindowPos();
            ListMax = ImVec2(ListMin.x + Width, ListMin.y + Height);
            const float TextY = (RowHeight - Layout.FontSize) * 0.5f;
            const ImVec2 Mouse = ImGui::GetIO().MousePos;

            for (int32 Row = 0; Row < Rows; ++Row)
            {
                const int32 Index = Completion.FirstRow + Row;
                const FCompletionEntry& Entry = Completion.Visible[Index];
                const FCodeCompletionItem& Item = Completion.Items[Entry.Item];
                const ImVec2 RowMin(Origin.x, Origin.y + Row * RowHeight);
                const ImVec2 RowMax(Origin.x + Inner, RowMin.y + RowHeight);
                const bool bRowHovered = bCompletionHovered && Mouse.x >= RowMin.x && Mouse.x < RowMax.x && Mouse.y >= RowMin.y && Mouse.y < RowMax.y;
                if (Index == Completion.Selected)
                {
                    DrawList->AddRectFilled(RowMin, RowMax, Palette.PopupSelected, 4.0f);
                }
                else if (bRowHovered)
                {
                    DrawList->AddRectFilled(RowMin, RowMax, EditorColors::ScaleAlpha(Palette.PopupSelected, 0.45f), 4.0f);
                }
                if (bRowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    Clicked = Index;
                }

                if (Item.Swatch != 0)
                {
                    const float Box = Layout.FontSize * 0.8f;
                    const ImVec2 BoxMin(RowMin.x + (IconWidth - Box) * 0.5f, RowMin.y + (RowHeight - Box) * 0.5f);
                    DrawList->AddRectFilled(BoxMin, ImVec2(BoxMin.x + Box, BoxMin.y + Box), Item.Swatch | IM_COL32_A_MASK, 2.0f);
                    DrawList->AddRect(BoxMin, ImVec2(BoxMin.x + Box, BoxMin.y + Box), Palette.PopupBorder, 2.0f);
                }
                else
                {
                    const char* Icon = KindIcon(Item.Kind);
                    const float IconX = RowMin.x + (IconWidth - ImGui::CalcTextSize(Icon).x) * 0.5f;
                    DrawList->AddText(ImVec2(IconX, RowMin.y + TextY), KindColor(Palette, Item.Kind), Icon);
                }

                const float LabelX = RowMin.x + IconWidth;
                DrawList->AddText(Layout.Font, Layout.FontSize, ImVec2(LabelX, RowMin.y + TextY), Palette[ECodeToken::Default], Item.Label.c_str());
                for (const uint16 Match : Entry.Matches)
                {
                    if (Match < Item.Label.size())
                    {
                        DrawList->AddText(Layout.Font, Layout.FontSize, ImVec2(LabelX + Match * Layout.CharWidth, RowMin.y + TextY), Palette.PopupMatch, Item.Label.c_str() + Match, Item.Label.c_str() + Match + 1);
                    }
                }

                if (!Item.Detail.empty())
                {
                    const float LabelEnd = LabelX + Item.Label.size() * Layout.CharWidth + Layout.CharWidth * 2.0f;
                    const float DetailWidthNeeded = Item.Detail.size() * Layout.CharWidth;
                    const float DetailX = Math::Max(LabelEnd, RowMax.x - Layout.CharWidth - DetailWidthNeeded);
                    DrawList->PushClipRect(ImVec2(LabelEnd, RowMin.y), ImVec2(RowMax.x - Layout.CharWidth * 0.5f, RowMax.y), true);
                    DrawList->AddText(Layout.Font, Layout.FontSize * 0.92f, ImVec2(DetailX, RowMin.y + TextY + Layout.FontSize * 0.04f), Palette.PopupDetail, Item.Detail.c_str());
                    DrawList->PopClipRect();
                }
            }

            if (Count > Rows)
            {
                const float TrackHeight = Rows * RowHeight;
                const float ThumbHeight = Math::Max(12.0f, TrackHeight * Rows / Count);
                const float ThumbTop = Origin.y + (TrackHeight - ThumbHeight) * Completion.FirstRow / (float)(Count - Rows);
                DrawList->AddRectFilled(ImVec2(Origin.x + Inner - 3.0f, ThumbTop), ImVec2(Origin.x + Inner - 1.0f, ThumbTop + ThumbHeight), Palette.PopupBorder, 1.0f);
            }
        }
        ImGui::End();

        const FCodeCompletionItem& Selected = Completion.Items[Completion.Visible[Math::Clamp(Completion.Selected, 0, Count - 1)].Item];
        if (!Selected.Documentation.empty())
        {
            const float DocWidth = 360.0f;
            ImVec2 DocPosition(ListMax.x + 4.0f, ListMin.y);
            if (DocPosition.x + DocWidth > Viewport->WorkPos.x + Viewport->WorkSize.x)
            {
                DocPosition.x = ListMin.x - DocWidth - 4.0f;
            }
            ImGui::SetNextWindowPos(DocPosition);
            ImGui::SetNextWindowSizeConstraints(ImVec2(DocWidth, 0.0f), ImVec2(DocWidth, 480.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
            if (ImGui::Begin("##CodeEditorCompletionDoc", nullptr, Flags | ImGuiWindowFlags_AlwaysAutoResize))
            {
                bCompletionHovered |= ImGui::IsWindowHovered();
                ImGui::PushTextWrapPos(DocWidth - 20.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, KindColor(Palette, Selected.Kind));
                ImGui::TextUnformatted(Selected.Label.c_str());
                ImGui::PopStyleColor();
                if (!Selected.Detail.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, Palette.PopupDetail);
                    ImGui::TextUnformatted(Selected.Detail.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::Spacing();
                PushUIFont();
                ImGui::PushStyleColor(ImGuiCol_Text, Palette[ECodeToken::Default]);
                ImGui::TextUnformatted(Selected.Documentation.c_str());
                ImGui::PopStyleColor();
                ImGuiX::Font::PopFont();
                ImGui::PopTextWrapPos();
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);

        if (Clicked >= 0)
        {
            Completion.Selected = Clicked;
            AcceptCompletion();
            Focus();
        }
    }

    void FCodeEditor::DrawSignaturePopup()
    {
        if (!bSignatureOpen || !bFocused || Signature.Label.empty())
        {
            return;
        }
        const FCodePosition Caret = Cursors[MainCursor].Head;
        const ImVec2 Anchor = ScreenAtPosition(Caret);
        ImGui::SetNextWindowPos(ImVec2(Anchor.x - Layout.CharWidth * 2.0f, Anchor.y - 3.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Palette.PopupBackground);
        ImGui::PushStyleColor(ImGuiCol_Border, Palette.PopupBorder);
        constexpr ImGuiWindowFlags Flags = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDocking;
        if (ImGui::Begin("##CodeEditorSignature", nullptr, Flags))
        {
            const FStringView Label = Signature.Label.View();
            const bool bHasActive = Signature.ActiveParameter >= 0 && Signature.ActiveParameter < (int32)Signature.Parameters.size();
            const FCodeSignature::FSpan Active = bHasActive ? Signature.Parameters[Signature.ActiveParameter] : FCodeSignature::FSpan{ (int32)Label.size(), (int32)Label.size() };
            auto Segment = [](FStringView Text, ImU32 Color)
            {
                if (Text.empty())
                {
                    return;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, Color);
                ImGui::TextUnformatted(Text.data(), Text.data() + Text.size());
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, 0.0f);
            };
            Segment(Label.substr(0, Active.Start), Palette[ECodeToken::Default]);
            Segment(Label.substr(Active.Start, Active.End - Active.Start), Palette.PopupMatch);
            Segment(Label.substr(Active.End), Palette[ECodeToken::Default]);
            ImGui::NewLine();
            if (!Signature.Documentation.empty())
            {
                PushUIFont();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, Palette.PopupDetail);
                ImGui::TextUnformatted(Signature.Documentation.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
                ImGuiX::Font::PopFont();
            }
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }

    void FCodeEditor::DrawHoverPopup()
    {
        if (!HoverState.bValid || Completion.bOpen || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        {
            return;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Palette.PopupBackground);
        ImGui::PushStyleColor(ImGuiCol_Border, Palette.PopupBorder);
        if (ImGui::BeginTooltip())
        {
            const FCodeHover& Hover = HoverState.Hover;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440.0f);
            if (Hover.Swatch != 0)
            {
                const ImVec2 Min = ImGui::GetCursorScreenPos();
                const ImVec2 Size(Layout.FontSize * 3.0f, Layout.FontSize * 1.4f);
                ImDrawList* DrawList = ImGui::GetWindowDrawList();
                DrawList->AddRectFilled(Min, ImVec2(Min.x + Size.x, Min.y + Size.y), IM_COL32(200, 200, 200, 255), 3.0f);
                DrawList->AddRectFilled(Min, ImVec2(Min.x + Size.x * 0.5f, Min.y + Size.y * 0.5f), IM_COL32(120, 120, 120, 255), 3.0f, ImDrawFlags_RoundCornersTopLeft);
                DrawList->AddRectFilled(ImVec2(Min.x + Size.x * 0.5f, Min.y + Size.y * 0.5f), ImVec2(Min.x + Size.x, Min.y + Size.y), IM_COL32(120, 120, 120, 255), 3.0f, ImDrawFlags_RoundCornersBottomRight);
                DrawList->AddRectFilled(Min, ImVec2(Min.x + Size.x, Min.y + Size.y), Hover.Swatch, 3.0f);
                DrawList->AddRect(Min, ImVec2(Min.x + Size.x, Min.y + Size.y), Palette.PopupBorder, 3.0f);
                ImGui::Dummy(Size);
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::AlignTextToFramePadding();
            }
            if (!Hover.Title.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Palette[ECodeToken::Function]);
                ImGui::TextUnformatted(Hover.Title.c_str());
                ImGui::PopStyleColor();
            }
            if (!Hover.Body.empty())
            {
                PushUIFont();
                ImGui::PushStyleColor(ImGuiCol_Text, Palette[ECodeToken::Default]);
                ImGui::TextUnformatted(Hover.Body.c_str());
                ImGui::PopStyleColor();
                ImGuiX::Font::PopFont();
            }
            if (!HoverState.DiagnosticText.empty())
            {
                if (!Hover.Title.empty() || !Hover.Body.empty())
                {
                    ImGui::Separator();
                }
                PushUIFont();
                ImGui::PushStyleColor(ImGuiCol_Text, HoverState.DiagnosticColor);
                ImGui::TextUnformatted(HoverState.DiagnosticText.c_str());
                ImGui::PopStyleColor();
                ImGuiX::Font::PopFont();
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }

    void FCodeEditor::DrawColorPicker()
    {
        if (ColorPick.bRequestOpen)
        {
            ImGui::OpenPopup("##CodeEditorColor");
            ColorPick.bRequestOpen = false;
        }
        if (!ImGui::BeginPopup("##CodeEditorColor"))
        {
            return;
        }
        PushUIFont();
        constexpr ImGuiColorEditFlags Flags = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_DisplayHex;
        if (ImGui::ColorPicker4("##CodeEditorColorPicker", ColorPick.Color, Flags))
        {
            const ImU32 Color = ImGui::ColorConvertFloat4ToU32(ImVec4(ColorPick.Color[0], ColorPick.Color[1], ColorPick.Color[2], ColorPick.Color[3]));
            const FString Text = CodeLanguageUtil::FormatHexColor(Color, ColorPick.Color[3] < 0.999f);
            BeginEdit(EEditKind::Typing);
            ReplaceRange(ColorPick.Range, Text.View());
            EndEdit();
            ColorPick.Range.End = { ColorPick.Range.Start.Line, ColorPick.Range.Start.Column + (int32)Text.size() };
        }
        ImGuiX::Font::PopFont();
        ImGui::EndPopup();
    }

    void FCodeEditor::DrawContextMenu()
    {
        if (!ImGui::BeginPopup("##CodeEditorContext"))
        {
            return;
        }
        PushUIFont();
        const bool bEditable = !Settings.bReadOnly;
        const bool bSelection = HasSelection();
        const CCodeEditorShortcuts& Keys = *GetDefault<CCodeEditorShortcuts>();
        auto Item = [](const char* Label, const SShortcut& Shortcut, bool bEnabled = true)
        {
            return ImGui::MenuItem(Label, Shortcut.GetLabel().c_str(), false, bEnabled);
        };
        if (Item(LE_ICON_UNDO " Undo", Keys.Undo, CanUndo())) { Undo(); }
        if (Item(LE_ICON_REDO " Redo", Keys.Redo, CanRedo())) { Redo(); }
        ImGui::Separator();
        if (Item(LE_ICON_CONTENT_CUT " Cut", Keys.Cut, bEditable)) { Cut(); }
        if (Item(LE_ICON_CONTENT_COPY " Copy", Keys.Copy)) { Copy(); }
        if (Item(LE_ICON_CONTENT_PASTE " Paste", Keys.Paste, bEditable)) { Paste(); }
        if (Item(LE_ICON_SELECT_ALL " Select All", Keys.SelectAll)) { SelectAll(); }
        ImGui::Separator();
        if (Item(LE_ICON_MAGNIFY " Find", Keys.Find)) { OpenFind(false); }
        if (Item(LE_ICON_FIND_REPLACE " Replace", Keys.Replace, bEditable)) { OpenFind(true); }
        if (Item(LE_ICON_CHECK_ALL " Select All Occurrences", Keys.SelectAllOccurrences)) { SelectAllOccurrences(); }
        ImGui::Separator();
        if (Item(LE_ICON_COMMENT_OUTLINE " Toggle Comment", Keys.ToggleLineComment, bEditable && Language != nullptr)) { ToggleLineComment(); }
        if (ImGui::MenuItem(LE_ICON_FORMAT_LETTER_CASE " Upper Case", nullptr, false, bEditable && bSelection)) { ChangeCase(true); }
        if (ImGui::MenuItem(LE_ICON_FORMAT_LETTER_CASE " Lower Case", nullptr, false, bEditable && bSelection)) { ChangeCase(false); }
        if (Item(LE_ICON_CODE_BRACES " Suggest", Keys.TriggerSuggestions, bEditable)) { TriggerCompletion(); }
        if (Item(LE_ICON_ARROW_RIGHT " Go to Definition", Keys.GoToDefinition)) { GoToDefinition(ContextMenuPosition); }
        if (CommandHandler)
        {
            if (Item(LE_ICON_FILE_FIND " Find All References", Keys.FindReferences)) { CommandHandler(ECommand::FindReferences, ContextMenuPosition); }
            if (Item(LE_ICON_RENAME " Rename Symbol", Keys.RenameSymbol, bEditable)) { CommandHandler(ECommand::Rename, ContextMenuPosition); }
        }
        if (Item(LE_ICON_LIGHTBULB_OUTLINE " Quick Fix", Keys.QuickFix, bEditable && Language != nullptr)) { ShowCodeActions(); }
        ImGui::Separator();
        if (Item(LE_ICON_FORMAT_ALIGN_LEFT " Format Document", Keys.FormatDocument, bEditable && Language != nullptr)) { FormatDocument(); }
        if (ImGui::MenuItem(LE_ICON_UNFOLD_LESS_HORIZONTAL " Fold All", nullptr, false, !Folds.empty())) { FoldAll(); }
        if (ImGui::MenuItem(LE_ICON_UNFOLD_MORE_HORIZONTAL " Unfold All", nullptr, false, !Collapsed.empty())) { UnfoldAll(); }
        ImGuiX::Font::PopFont();
        ImGui::EndPopup();
    }

    void FCodeEditor::DrawCodeActionsPopup()
    {
        if (bOpenCodeActions)
        {
            bOpenCodeActions = false;
            const ImVec2 At = ScreenAtPosition(Cursors[MainCursor].Head);
            ImGui::SetNextWindowPos(ImVec2(At.x, At.y + Layout.LineHeight));
            ImGui::OpenPopup("##CodeActions");
        }
        if (!ImGui::BeginPopup("##CodeActions"))
        {
            return;
        }
        PushUIFont();
        if (CodeActions.empty())
        {
            ImGui::TextDisabled("No quick fixes here");
        }
        for (int32 Index = 0; Index < (int32)CodeActions.size(); ++Index)
        {
            ImGui::PushID(Index);
            if (ImGui::MenuItem((FString(LE_ICON_LIGHTBULB_OUTLINE " ") + CodeActions[Index].Title).c_str()))
            {
                const FCodeAction Chosen = CodeActions[Index];
                ApplyEdits(Chosen.Edits);
                Focus();
            }
            ImGui::PopID();
        }
        ImGuiX::Font::PopFont();
        ImGui::EndPopup();
    }

    void FCodeEditor::DrawFindBar(float Width)
    {
        const ImGuiStyle& Style = ImGui::GetStyle();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorColors::PanelBg());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
        if (ImGui::BeginChild("##find", ImVec2(Width, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            UpdateFindMatches();
            const float ButtonWidth = ImGui::GetFrameHeight();

            if (ImGui::Button(Find.bReplace ? LE_ICON_CHEVRON_DOWN : LE_ICON_CHEVRON_RIGHT, ImVec2(ButtonWidth, 0.0f)))
            {
                Find.bReplace = !Find.bReplace;
            }
            ImGui::SetItemTooltip("Toggle replace");
            ImGui::SameLine();

            const float Trailing = ButtonWidth * 5.0f + Style.ItemSpacing.x * 5.0f + ImGui::CalcTextSize("0000 of 0000").x;
            ImGui::SetNextItemWidth(Math::Max(80.0f, ImGui::GetContentRegionAvail().x - Trailing));
            if (Find.bFocusQuery)
            {
                ImGui::SetKeyboardFocusHere();
                Find.bFocusQuery = false;
            }
            const bool bSubmit = ImGui::InputTextWithHint("##query", "Find", Find.Query, sizeof(Find.Query), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (bSubmit)
            {
                FindNext(ImGui::GetIO().KeyShift);
                ImGui::SetKeyboardFocusHere(-1);
            }

            auto Toggle = [ButtonWidth](const char* Label, bool& bValue, const char* Tooltip)
            {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, bValue ? EditorColors::WithAlpha(EditorColors::Accent(), 0.45f) : EditorColors::Button());
                if (ImGui::Button(Label, ImVec2(ButtonWidth, 0.0f)))
                {
                    bValue = !bValue;
                }
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("%s", Tooltip);
            };
            Toggle(LE_ICON_FORMAT_LETTER_CASE, Find.bMatchCase, "Match case");
            Toggle(LE_ICON_ALPHA_W_BOX_OUTLINE, Find.bWholeWord, "Whole word");

            ImGui::SameLine();
            int32 Current = 0;
            const FCodeRange Selection = Cursors[MainCursor].GetRange();
            for (int32 i = 0; i < (int32)Find.Matches.size(); ++i)
            {
                if (Find.Matches[i] == Selection)
                {
                    Current = i + 1;
                    break;
                }
            }
            if (Find.Query[0] == '\0')
            {
                ImGui::TextDisabled("              ");
            }
            else if (Find.Matches.empty())
            {
                ImGui::TextColored(EditorColors::Danger(), "No results");
            }
            else
            {
                ImGui::TextDisabled("%d of %d", Current, (int32)Find.Matches.size());
            }

            ImGui::SameLine();
            if (ImGui::Button(LE_ICON_ARROW_UP, ImVec2(ButtonWidth, 0.0f)))
            {
                FindNext(true);
            }
            ImGui::SetItemTooltip("Previous match (Shift+F3)");
            ImGui::SameLine();
            if (ImGui::Button(LE_ICON_ARROW_DOWN, ImVec2(ButtonWidth, 0.0f)))
            {
                FindNext(false);
            }
            ImGui::SetItemTooltip("Next match (F3)");
            ImGui::SameLine();
            if (ImGui::Button(LE_ICON_CLOSE, ImVec2(ButtonWidth, 0.0f)))
            {
                Find.bOpen = false;
                Focus();
            }

            if (Find.bReplace)
            {
                ImGui::Dummy(ImVec2(ButtonWidth, 0.0f));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(Math::Max(80.0f, ImGui::GetContentRegionAvail().x - Trailing));
                ImGui::InputTextWithHint("##replace", "Replace", Find.Replacement, sizeof(Find.Replacement));
                ImGui::SameLine();
                ImGui::BeginDisabled(Settings.bReadOnly || Find.Matches.empty());
                if (ImGui::Button("Replace"))
                {
                    if (Current > 0)
                    {
                        BeginEdit(EEditKind::Other);
                        ReplaceRange(Selection, FStringView(Find.Replacement));
                        EndEdit();
                    }
                    FindNext(false);
                }
                ImGui::SameLine();
                if (ImGui::Button("All"))
                {
                    ReplaceAll();
                }
                ImGui::SetItemTooltip("Replace every match");
                ImGui::EndDisabled();
            }

            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && GetDefault<CCodeEditorShortcuts>()->Dismiss.IsPressed(0, ImGuiInputFlags_RouteAlways))
            {
                Find.bOpen = false;
                Focus();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
}
