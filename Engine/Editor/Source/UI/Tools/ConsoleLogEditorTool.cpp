#include "ConsoleLogEditorTool.h"
#include "Config/Config.h"
#include "Core/Math/Math.h"
#include "Settings/EditorSettings.h"
#include "UI/EditorUI.h"
#include "Tools/UI/ImGui/EditorColors.h"

#include <utility>

#include "Core/Console/ConsoleVariable.h"
#include "Log/LogMessage.h"
#include "Log/Log.h"

namespace Lumina
{
    constexpr float kInputOverlayGap = 5.0f;

    namespace
    {
        // Recognizes both a managed stack frame and a Roslyn diagnostic, which spell the line number differently.
        bool TryParseSourceLocation(FStringView Text, FString& OutPath, int32& OutLine)
        {
            const FStringView Extension(".cs");
            for (int32 Dot = Text.Find(Extension); Dot != Constants::kIndexNone; Dot = Text.Find(Extension, Dot + 1))
            {
                const int32 PathEnd = Dot + Extension.Len();

                int32 LineStart = Constants::kIndexNone;
                if (Text.Mid(PathEnd).StartsWith(FStringView(":line ")))
                {
                    LineStart = PathEnd + 6;
                }
                else if (Text.Mid(PathEnd).StartsWith('('))
                {
                    LineStart = PathEnd + 1;
                }
                if (LineStart == Constants::kIndexNone)
                {
                    continue;
                }

                int32 Line = 0;
                int32 Digit = LineStart;
                for (; Digit < Text.Len() && Text[Digit] >= '0' && Text[Digit] <= '9'; ++Digit)
                {
                    Line = Line * 10 + (Text[Digit] - '0');
                }
                if (Digit == LineStart)
                {
                    continue;
                }

                const int32 Newline = Text.FindLast('\n', Dot);
                const int32 LineBegin = Newline == Constants::kIndexNone ? 0 : Newline + 1;

                // Script paths are absolute, so the start is a drive anchor or the first separator on the line.
                int32 PathStart = Constants::kIndexNone;
                for (int32 Index = Dot; Index-- > LineBegin && Index + 2 < Text.Len(); )
                {
                    if (Text[Index + 1] == ':' && (Text[Index + 2] == '\\' || Text[Index + 2] == '/'))
                    {
                        PathStart = Index;
                        break;
                    }
                }
                if (PathStart == Constants::kIndexNone)
                {
                    PathStart = Text.Find('/', LineBegin);
                }
                if (PathStart == Constants::kIndexNone || PathStart > Dot)
                {
                    continue;
                }

                const FStringView Path = Text.Mid(PathStart, PathEnd - PathStart);
                OutPath = FStringView(Path.GetData(), Path.Len());
                OutLine = Line;
                return true;
            }
            return false;
        }
    }
    void FConsoleLogEditorTool::OnInitialize()
    {
        CreateToolWindow("Console", [&] (bool bIsFocused)
        {
            DrawLogWindow(bIsFocused);
        });

        // Seeded from the queue rather than a literal, so the slider opens showing what is really retained.
        Settings.MaxMessageCount = (int32)Logging::GetConsoleLogQueueCapacity();
    }

    void FConsoleLogEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        
    }

    void FConsoleLogEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Filtering",
            "Use the Filter menu to hide log levels you don't care about. The text filter at the top "
            "supports inclusive (foo) and exclusive (-bar) terms separated by commas.");
        DrawHelpTextRow("Console Commands",
            "Type a console variable or command at the bottom prompt; Tab/Up navigates autocomplete. "
            "Up/Down with empty input cycles command history. The backtick key opens the log with the "
            "prompt focused, and pressing it again from the prompt closes the drawer.");
        DrawHelpTextRow("Autocomplete",
            "Live-matched against registered CVars and exec commands. Description text comes from the "
            "FAutoConsoleVariable / FAutoConsoleCommand registration site.");
        DrawHelpTextRow("Selecting & Copying",
            "Click a line to select it, shift-click for a range, ctrl-click to toggle one. Ctrl+C copies "
            "the selection, Ctrl+A selects everything visible, Esc clears. Right-click for the same "
            "options plus Copy All Visible.");
        DrawHelpTextRow("Export",
            "Copies the currently filtered messages out to a text file.");
        DrawHelpTextRow("Clear",
            "Empties the engine's console message queue. The log file on disk keeps everything.");
        DrawHelpTextRow("Max Messages",
            "Size of that queue. The oldest messages fall off once it is full, and lowering the limit "
            "discards them immediately. It does not affect what is written to the log file.");
    }

    void FConsoleLogEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        if (ImGui::BeginMenu(LE_ICON_FILTER " Filter"))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 3));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));

            bool bFilterChanged = false;

            ImGui::SeparatorText("Log Levels");
            bFilterChanged |= ImGui::Checkbox("Trace", &Filter.bShowTrace);
            bFilterChanged |= ImGui::Checkbox("Debug", &Filter.bShowDebug);
            bFilterChanged |= ImGui::Checkbox("Info", &Filter.bShowInfo);
            bFilterChanged |= ImGui::Checkbox("Warning", &Filter.bShowWarning);
            bFilterChanged |= ImGui::Checkbox("Error", &Filter.bShowError);
            bFilterChanged |= ImGui::Checkbox("Critical", &Filter.bShowCritical);

            ImGui::Spacing();

            if (ImGui::Button("All", ImVec2(70, 0)))
            {
                Filter.bShowTrace = Filter.bShowDebug = Filter.bShowInfo = 
                Filter.bShowWarning = Filter.bShowError = Filter.bShowCritical = true;
                bFilterChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("None", ImVec2(70, 0)))
            {
                Filter.bShowTrace = Filter.bShowDebug = Filter.bShowInfo = 
                Filter.bShowWarning = Filter.bShowError = Filter.bShowCritical = false;
                bFilterChanged = true;
            }

            ImGui::SameLine();
            if (ImGui::Button("Errors Only", ImVec2(70, 0)))
            {
                Filter.bShowTrace = Filter.bShowDebug = Filter.bShowInfo = Filter.bShowWarning = false;
                Filter.bShowError = Filter.bShowCritical = true;
                bFilterChanged = true;
            }

            ImGui::PopStyleVar(2);
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu(LE_ICON_COG " Settings"))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 3));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
            
            ImGui::SeparatorText("Display");
            ImGui::Checkbox("Auto Scroll", &Settings.bAutoScroll);
            ImGui::Checkbox("Show Timestamps", &Settings.bShowTimestamps);
            ImGui::Checkbox("Show Logger", &Settings.bShowLogger);
            ImGui::Checkbox("Show Icons", &Settings.bShowIcons);
            ImGui::Checkbox("Word Wrap", &Settings.bWordWrap);

            CWorldEditorSettings* WorldSettings = GetMutableDefault<CWorldEditorSettings>();
            if (ImGui::Checkbox("Clear on Play", &WorldSettings->bClearConsoleOnPIE))
            {
                Global::GConfig->SaveSettings(CWorldEditorSettings::StaticClass());
            }
            ImGuiX::TextTooltip("Empty the console each time play in editor starts. Also in Editor Settings, World Tool.");

            ImGui::Spacing();
            ImGui::SeparatorText("Performance");
            
            if (ImGui::SliderFloat("Font Scale", &Settings.FontScale, 0.7f, 2.0f, "%.1f"))
            {
                
            }

            if (ImGui::SliderInt("Max Messages", &Settings.MaxMessageCount, 100, 2500))
            {
                Logging::SetConsoleLogQueueCapacity((uint32)Settings.MaxMessageCount);
            }
            ImGuiX::TextTooltip("How many messages the console keeps. Lowering it drops the oldest immediately.");

            ImGui::Spacing();
            ImGui::SeparatorText("Actions");
            
            if (ImGui::Button("Clear Console", ImVec2(-1, 0)))
            {
                ClearConsole();
            }

            if (ImGui::Button("Export Logs", ImVec2(-1, 0)))
            {
                ExportLogs("console_log.txt");
            }

            ImGui::PopStyleVar(2);
            ImGui::EndMenu();
        }

        ImGui::Separator();
        ImGui::Text("Messages: %d / %d", VisibleRows.Indices.Num(), PreviousMessageSize);
    }

    void FConsoleLogEditorTool::DrawLogWindow(bool bIsFocused)
    {
        const float InputHeight = ImGui::GetFrameHeightWithSpacing() * 1.2f;
        
        ImGuiX::SearchBar("##LogFilter", Filter.TextFilter, LE_ICON_MAGNIFY " Filter messages...");

        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorColors::FrameBg());
        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextPrimary());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));

        const float LogHeight = ImGui::GetContentRegionAvail().y - InputHeight;
        LogTopY = ImGui::GetCursorScreenPos().y;
        ImGui::BeginChild("##LogMessages", ImVec2(0, LogHeight), true,
            ImGuiWindowFlags_HorizontalScrollbar);

        // Sticking keys off the scroll position, since the ring's size stops growing once saturated.
        const bool bWasPinnedToBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

        const Logging::FLogQueue& Messages = Logging::GetConsoleLogQueue();
        // A clear from anywhere, such as play starting, leaves the selected queue indices pointing at new lines.
        if (Messages.Num() < PreviousMessageSize)
        {
            SelectedMessages.Reset();
            SelectionAnchor = -1;
        }
        PreviousMessageSize = Messages.Num();

        if (Settings.FontScale != 1.0f)
        {
            ImGui::PushFontSize(ImGui::GetFontSize() * Settings.FontScale);
        }

        VisibleRows.Refresh(Messages, Filter);
        const TVector<uint32>& VisibleIndices = VisibleRows.Indices;

        // Wrap disables clipper; full iteration is still cheaper than a table.
        const bool bUseClipper = !Settings.bWordWrap;

        // Splitting the draw list lets the selection rect land behind text measured after the fact.
        ImDrawList* RowDrawList = ImGui::GetWindowDrawList();
        RowDrawList->ChannelsSplit(2);
        RowDrawList->ChannelsSetCurrent(1);

        const bool bLogHovered = ImGui::IsWindowHovered();
        const float RowRight = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;

        auto DrawLine = [&](uint32 Index)
        {
            const FConsoleMessage& Message = Messages[Index];
            const ImVec4 Color = GetColorForLevel(Message.Level);

            const ImVec2 RowMin = ImGui::GetCursorScreenPos();

            if (Settings.bShowTimestamps)
            {
                ImGui::TextColored(EditorColors::TextMuted(), "[%s]", *Message.Time);
                ImGui::SameLine(0, 6);
            }

            if (Settings.bShowIcons)
            {
                ImGui::TextColored(Color, "%s", GetLevelIcon(Message.Level));
                ImGui::SameLine(0, 4);
            }

            if (Settings.bShowLogger)
            {
                ImGui::TextColored(EditorColors::TextDim(), "%s:", Message.LoggerName.GetData());
                ImGui::SameLine(0, 6);
            }

            if (Settings.bWordWrap)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Color);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(*Message.Message);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextColored(Color, "%s", *Message.Message);
            }

            const ImVec2 RowMax(RowRight, ImGui::GetCursorScreenPos().y);

            if (SelectedMessages.Contains(Index))
            {
                RowDrawList->ChannelsSetCurrent(0);
                RowDrawList->AddRectFilled(RowMin, RowMax, EditorColors::U32(EditorColors::WithAlpha(EditorColors::Accent(), 0.35f)));
                RowDrawList->ChannelsSetCurrent(1);
            }

            if (bLogHovered && ImGui::IsMouseHoveringRect(RowMin, RowMax))
            {
                FString SourcePath;
                int32   SourceLine = 0;
                const bool bHasSource = TryParseSourceLocation(Message.Message, SourcePath, SourceLine);

                if (bHasSource)
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Double-click to open %s at line %d", *SourcePath, SourceLine);
                }

                if (bHasSource && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    ToolContext->OpenScriptInExternalEditor(FStringView(*SourcePath, SourcePath.Len()), SourceLine);
                }
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    HandleRowClick(Index, VisibleIndices);
                }
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                    && !SelectedMessages.Contains(Index))
                {
                    // Right-clicking outside the selection retargets it, matching every list view.
                    HandleRowClick(Index, VisibleIndices);
                }
            }
        };

        if (bUseClipper)
        {
            ImGuiListClipper Clipper;
            Clipper.Begin(VisibleIndices.Num());
            while (Clipper.Step())
            {
                for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
                {
                    DrawLine(VisibleIndices[Row]);
                }
            }
            Clipper.End();
        }
        else
        {
            for (uint32 Idx : VisibleIndices)
            {
                DrawLine(Idx);
            }
        }

        RowDrawList->ChannelsMerge();

        // The log child never takes focus while the command input holds it, so hovering is the only test.
        if (bLogHovered && !ImGui::GetIO().WantTextInput)
        {
            const ImGuiIO& IO = ImGui::GetIO();

            if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
            {
                CopyToClipboard(VisibleIndices, !SelectedMessages.IsEmpty());
            }

            if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))
            {
                SelectedMessages.Reset();
                for (uint32 Visible : VisibleIndices)
                {
                    SelectedMessages.Add(Visible);
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                SelectedMessages.Reset();
                SelectionAnchor = -1;
            }
        }

        if (ImGui::BeginPopupContextWindow("##LogContext"))
        {
            const int32 SelectedCount = SelectedMessages.Num();

            if (ImGui::MenuItem("Copy", "Ctrl+C", false, SelectedCount > 0))
            {
                CopyToClipboard(VisibleIndices, true);
            }

            if (ImGui::MenuItem("Copy All Visible"))
            {
                CopyToClipboard(VisibleIndices, false);
            }

            FString SourcePath;
            int32   SourceLine = 0;
            for (uint32 Selected : SelectedMessages)
            {
                if (Selected < static_cast<uint32>(Messages.Num()) && TryParseSourceLocation(Messages[Selected].Message, SourcePath, SourceLine))
                {
                    break;
                }
            }

            if (ImGui::MenuItem("Open Source", nullptr, false, SourceLine > 0))
            {
                ToolContext->OpenScriptInExternalEditor(FStringView(*SourcePath, SourcePath.Len()), SourceLine);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Select All", "Ctrl+A"))
            {
                SelectedMessages.Reset();
                for (uint32 Visible : VisibleIndices)
                {
                    SelectedMessages.Add(Visible);
                }
            }

            if (ImGui::MenuItem("Clear Selection", "Esc", false, SelectedCount > 0))
            {
                SelectedMessages.Reset();
                SelectionAnchor = -1;
            }

            ImGui::EndPopup();
        }

        if (bNeedsScrollToBottom || (Settings.bAutoScroll && bWasPinnedToBottom))
        {
            ImGui::SetScrollHereY(1.0f);
            bNeedsScrollToBottom = false;
        }

        if (Settings.FontScale != 1.0f)
        {
            ImGui::PopFontSize();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorColors::FrameBg());
        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextPrimary());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));

        ImGui::TextColored(EditorColors::Success(), ">");
        ImGui::SameLine(0, 6);
        ImGui::SetNextItemWidth(-1);

        if (bFocusInput)
        {
            ImGui::SetKeyboardFocusHere();
            bFocusInput = false;
        }

        const ImGuiInputTextFlags InputFlags =
            ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_CallbackCompletion |
            ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_CallbackEdit |
            ImGuiInputTextFlags_CallbackCharFilter |
            ImGuiInputTextFlags_CallbackAlways;

        const bool bExecuteCommand = ImGui::InputTextWithHint(
            "##CommandInput",
            "enter command or console variable...",
            InputBuffer,
            sizeof(InputBuffer),
            InputFlags,
            &FConsoleLogEditorTool::InputTextCallbackStub,
            this);

        const bool bInputActive = ImGui::IsItemActive();
        InputRectMin = ImGui::GetItemRectMin();
        InputWidth   = ImGui::GetItemRectSize().x;
        if (bInputActive)
        {
            CommandInputActiveFrame = ImGui::GetFrameCount();
        }

        if (bExecuteCommand)
        {
            FStringView Command(InputBuffer);
            if (!Command.IsEmpty())
            {
                ProcessCommand(Command);
                AddCommandToHistory(Command);
                InputBuffer[0] = '\0';
                bNeedsScrollToBottom = true;
                bShowAutoComplete = false;
                AutoCompleteCandidates.Reset();
            }
            bFocusInput = true;
        }

        if (bInputActive && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            if (bShowAutoComplete)
            {
                bShowAutoComplete = false;
                AutoCompleteCandidates.Reset();
            }
            else if (bShowHistory)
            {
                bShowHistory = false;
            }
            else if (InputBuffer[0] != '\0')
            {
                InputBuffer[0] = '\0';
            }
        }

        if (bInputActive && ImGui::IsKeyPressed(ImGuiKey_Space) && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper))
        {
            bShowHistory = !bShowHistory;
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (bShowAutoComplete && !AutoCompleteCandidates.IsEmpty())
        {
            DrawAutoCompletePopup();
        }

        if (bShowHistory)
        {
            DrawHistoryPopup();
        }
    }

    bool FConsoleLogEditorTool::IsCommandInputActive() const
    {
        return CommandInputActiveFrame >= ImGui::GetFrameCount() - 1;
    }

    float FConsoleLogEditorTool::GetInputOverlayMaxHeight() const
    {
        return Math::Max(InputRectMin.y - kInputOverlayGap - LogTopY, ImGui::GetFrameHeight());
    }

    int FConsoleLogEditorTool::InputTextCallbackStub(ImGuiInputTextCallbackData* Data)
    {
        FConsoleLogEditorTool* Self = static_cast<FConsoleLogEditorTool*>(Data->UserData);
        return Self->InputTextCallback(Data);
    }

    int FConsoleLogEditorTool::InputTextCallback(ImGuiInputTextCallbackData* Data)
    {
        switch (Data->EventFlag)
        {
            case ImGuiInputTextFlags_CallbackCharFilter:
            {
                // The backtick toggles the console, so it never lands in the command.
                return Data->EventChar == '`' ? 1 : 0;
            }
            case ImGuiInputTextFlags_CallbackAlways:
            {
                if (bPendingBufferReplacement)
                {
                    Data->DeleteChars(0, Data->BufTextLen);
                    Data->InsertChars(0, *PendingBufferReplacement);
                    Data->CursorPos = Data->BufTextLen;
                    Data->SelectionStart = Data->SelectionEnd = Data->CursorPos;
                    bPendingBufferReplacement = false;
                    PendingBufferReplacement.Reset();
                }
                break;
            }
            case ImGuiInputTextFlags_CallbackEdit:
            {
                FStringView Current(Data->Buf, (size_t)Data->BufTextLen);
                UpdateAutoComplete(Current);
                break;
            }
            case ImGuiInputTextFlags_CallbackCompletion:
            {
                if (bShowAutoComplete && !AutoCompleteCandidates.IsEmpty())
                {
                    int32 Index = AutoCompleteSelectedIndex;
                    if (Index < 0 || Index >= AutoCompleteCandidates.Num())
                    {
                        Index = 0;
                    }

                    FStringView Replacement = AutoCompleteCandidates[Index].Name;
                    Data->DeleteChars(0, Data->BufTextLen);
                    Data->InsertChars(0, Replacement.GetData(), Replacement.GetData() + Replacement.Len());
                    Data->CursorPos = Data->BufTextLen;
                    bShowAutoComplete = false;
                    AutoCompleteCandidates.Reset();
                }
                break;
            }
            case ImGuiInputTextFlags_CallbackHistory:
            {
                if (bShowAutoComplete && !AutoCompleteCandidates.IsEmpty())
                {
                    if (Data->EventKey == ImGuiKey_DownArrow)
                    {
                        AutoCompleteSelectedIndex = (AutoCompleteSelectedIndex + 1) % AutoCompleteCandidates.Num();
                    }
                    else if (Data->EventKey == ImGuiKey_UpArrow)
                    {
                        AutoCompleteSelectedIndex--;
                        if (AutoCompleteSelectedIndex < 0)
                        {
                            AutoCompleteSelectedIndex = AutoCompleteCandidates.Num() - 1;
                        }
                    }
                }
                else
                {
                    const uint64 PrevHistoryIndex = HistoryIndex;
                    if (Data->EventKey == ImGuiKey_UpArrow)
                    {
                        NavigateHistory(-1);
                    }
                    else if (Data->EventKey == ImGuiKey_DownArrow)
                    {
                        NavigateHistory(1);
                    }

                    if (HistoryIndex != PrevHistoryIndex)
                    {
                        const FString& Replacement = (HistoryIndex < static_cast<uint64>(CommandHistory.Num()))
                            ? CommandHistory[static_cast<int32>(HistoryIndex)]
                            : FString();
                        Data->DeleteChars(0, Data->BufTextLen);
                        if (!Replacement.IsEmpty())
                        {
                            Data->InsertChars(0, *Replacement);
                        }
                        Data->CursorPos = Data->BufTextLen;
                    }
                }
                break;
            }
            default: break;
        }
        return 0;
    }

    void FConsoleLogEditorTool::ApplyCompletion(FStringView Replacement)
    {
        PendingBufferReplacement = FStringView(Replacement.GetData(), Replacement.Len());
        bPendingBufferReplacement = true;
        bShowAutoComplete = false;
        AutoCompleteCandidates.Reset();
        bFocusInput = true;
    }

    ImVec4 FConsoleLogEditorTool::GetColorForLevel(ELogLevel Level)
    {
        switch (Level)
        {
            case ELogLevel::Error:
            case ELogLevel::Critical: return EditorColors::Danger();
            case ELogLevel::Warn:     return EditorColors::Warning();
            case ELogLevel::Info:     return EditorColors::TextPrimary();
            case ELogLevel::Debug:    return EditorColors::Accent();
            case ELogLevel::Trace:    return EditorColors::TextMuted();
            default:                  return EditorColors::TextDim();
        }
    }

    void FConsoleLogEditorTool::ProcessCommand(FStringView Command)
    {
        FConsoleRegistry::Get().ExecuteLine(Command, true);
    }

    void FConsoleLogEditorTool::AddCommandToHistory(FStringView Command)
    {
        if (!CommandHistory.IsEmpty() && CommandHistory.Last() == Command)
        {
            HistoryIndex = CommandHistory.Num();
            return;
        }

        CommandHistory.PushLast(FString(Command));

        constexpr int32 MaxHistory = 100;
        if (CommandHistory.Num() > MaxHistory)
        {
            CommandHistory.PopFirst();
        }
        
        HistoryIndex = CommandHistory.Num();
    }

    void FConsoleLogEditorTool::NavigateHistory(int32 Direction)
    {
        if (CommandHistory.IsEmpty())
        {
            return;
        }

        if (Direction < 0)
        {
            if (HistoryIndex > 0)
            {
                HistoryIndex--;
            }
        }
        else if (Direction > 0)
        {
            if (HistoryIndex < static_cast<uint64>(CommandHistory.Num()))
            {
                HistoryIndex++;
            }
        }
    }

    void FConsoleLogEditorTool::ClearConsole()
    {
        Logging::ClearLogQueue();
        bNeedsScrollToBottom = false;
    }

    void FConsoleLogEditorTool::ExportLogs(const FString& FilePath)
    {
        LOG_INFO("Exporting logs to: {}", FilePath);
    }

    void FConsoleLogEditorTool::UpdateAutoComplete(FStringView CurrentInput)
    {
        AutoCompleteCandidates.Reset();
        AutoCompleteSelectedIndex = 0;

        if (CurrentInput.IsEmpty())
        {
            bShowAutoComplete = false;
            return;
        }
        
        FConsoleRegistry& Registry = FConsoleRegistry::Get();

        for (const auto& [Name, Var] : Registry.GetAll())
        {
            float Score = CalculateMatchScore(Name, CurrentInput);
            if (Score > 0.0f)
            {
                TOptional<FString> Value = Registry.GetValueAsString(Name);

                if (Value.IsSet())
                {
                    AutoCompleteCandidates.Emplace(Name, Var.Hint, Value.GetValue(), Score);
                }
            }
        }

        for (const auto& [Name, Cmd] : Registry.GetAllCommands())
        {
            float Score = CalculateMatchScore(Name, CurrentInput);
            if (Score > 0.0f)
            {
                AutoCompleteCandidates.Emplace(Name, Cmd.Hint, FString(), Score);
            }
        }

        Algo::Sort(AutoCompleteCandidates, [](const FAutoCompleteCandidate& A, const FAutoCompleteCandidate& B)
        {
            return A.MatchScore > B.MatchScore;
        });

        if (AutoCompleteCandidates.Num() > 10)
        {
            AutoCompleteCandidates.SetNum(10);
        }

        bShowAutoComplete = !AutoCompleteCandidates.IsEmpty();
    }

    void FConsoleLogEditorTool::DrawAutoCompletePopup()
    {
        // Bottom-anchored on last frame's height, since a child only learns its auto size after drawing once.
        const float MaxHeight = GetInputOverlayMaxHeight();
        const float Height = Math::Min(AutoCompleteOverlayHeight > 0.0f ? AutoCompleteOverlayHeight : MaxHeight, MaxHeight);
        ImGui::SetCursorScreenPos(ImVec2(InputRectMin.x, InputRectMin.y - kInputOverlayGap - Height));
        ImGui::SetNextWindowSizeConstraints(ImVec2(InputWidth, 0.0f), ImVec2(InputWidth, MaxHeight));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorColors::PanelBg());
        ImGui::PushStyleColor(ImGuiCol_Border, EditorColors::Border());

        if (ImGui::BeginChild("##AutoComplete", ImVec2(InputWidth, 0.0f),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoNavFocus))
        {
            ImGui::TextColored(EditorColors::TextDim(), "Auto-Complete (%d matches)", AutoCompleteCandidates.Num());
            ImGui::Separator();

            for (int32 i = 0; i < AutoCompleteCandidates.Num(); ++i)
            {
                const FAutoCompleteCandidate& Candidate = AutoCompleteCandidates[i];
                
                bool bIsSelected = (i == AutoCompleteSelectedIndex);
                
                if (bIsSelected)
                {
                    ImGui::PushStyleColor(ImGuiCol_Header, EditorColors::WithAlpha(EditorColors::Accent(), 0.8f));
                }

                ImGui::PushID(i);
                
                if (ImGui::Selectable("##Candidate", bIsSelected, 0, ImVec2(0, 0)))
                {
                    ApplyCompletion(Candidate.Name);
                }

                ImGui::SameLine(0, 4);

                ImGui::TextColored(EditorColors::TextPrimary(), "%s", Candidate.Name.GetData());
                
                if (!Candidate.CurrentValue.IsEmpty())
                {
                    ImGui::SameLine();
                    ImGui::TextColored(EditorColors::Accent(), "= %s", *Candidate.CurrentValue);
                }

                if (!Candidate.Description.IsEmpty())
                {
                    ImGui::Indent(20.0f);
                    ImGui::TextColored(EditorColors::TextMuted(), "%s", Candidate.Description.GetData());
                    ImGui::Unindent(20.0f);
                }

                ImGui::PopID();

                if (bIsSelected)
                {
                    ImGui::PopStyleColor();
                }
            }

            ImGui::Separator();
            ImGui::TextColored(EditorColors::TextMuted(), LE_ICON_ARROW_UP_DOWN " Navigate  " LE_ICON_KEYBOARD_TAB " Accept  " LE_ICON_KEYBOARD_ESC " Cancel");
        }
        AutoCompleteOverlayHeight = ImGui::GetWindowHeight();
        ImGui::EndChild();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    void FConsoleLogEditorTool::DrawHistoryPopup()
    {
        constexpr float kHistoryHeight = 300.0f;
        const float Height = Math::Min(kHistoryHeight, GetInputOverlayMaxHeight());
        const ImVec2 HistoryMin(InputRectMin.x, InputRectMin.y - kInputOverlayGap - Height);
        ImGui::SetCursorScreenPos(HistoryMin);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 3));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorColors::PanelBg());
        ImGui::PushStyleColor(ImGuiCol_Border, EditorColors::Border());

        if (ImGui::BeginChild("##CommandHistory", ImVec2(InputWidth, Height),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            ImGui::TextColored(EditorColors::TextDim(), LE_ICON_HISTORY " Command History (%d)", CommandHistory.Num());
            
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear"))
            {
                CommandHistory.Reset();
                HistoryIndex = 0;
            }

            ImGui::Separator();

            ImGui::BeginChild("##HistoryList", ImVec2(0, 0), false);

            if (CommandHistory.IsEmpty())
            {
                ImGui::TextColored(EditorColors::TextMuted(), "No command history");
            }
            else
            {
                for (int32 i = CommandHistory.Num() - 1; i >= 0; --i)
                {
                    const FString& Cmd = CommandHistory[i];
                    
                    ImGui::PushID(i);
                    
                    bool bIsSelected = (i == (int32)HistoryIndex);
                    
                    if (ImGui::Selectable(*Cmd, bIsSelected))
                    {
                        ApplyCompletion(Cmd);
                        bShowHistory = false;
                    }


                    ImGui::PopID();
                }
            }

            ImGui::EndChild();
        }
        ImGui::EndChild();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        const ImVec2 HistoryMax(HistoryMin.x + InputWidth, HistoryMin.y + Height);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseHoveringRect(HistoryMin, HistoryMax, false))
        {
            bShowHistory = false;
        }
    }
    
    float FConsoleLogEditorTool::CalculateMatchScore(FStringView Candidate, FStringView Input)
    {
        if (Candidate.IsEmpty() || Input.IsEmpty())
        {
            return 0.0f;
        }
        
        if (Candidate.StartsWith(Input))
        {
            return 1.0f;
        }

        int32 Pos = Candidate.Find(Input);
        if (Pos != Constants::kIndexNone)
        {
            return 0.7f - static_cast<float>(Pos) / static_cast<float>(Input.Len()) * 0.2f;
        }

        int32 CandidatePos = 0;
        int32 MatchedChars = 0;
        
        for (int32 i = 0; i < Input.Len() && CandidatePos < Candidate.Len(); ++i)
        {
            char InputChar = (char)std::tolower((unsigned char)Input[i]);
            while (CandidatePos < Candidate.Len())
            {
                if (std::tolower(Candidate[CandidatePos]) == InputChar)
                {
                    MatchedChars++;
                    CandidatePos++;
                    break;
                }
                CandidatePos++;
            }
        }

        if (MatchedChars == Input.Len())
        {
            return 0.5f * static_cast<float>(MatchedChars) / static_cast<float>(Candidate.Len());
        }

        return 0.0f;
    }
    
    void FConsoleLogEditorTool::HandleRowClick(uint32 MessageIndex, const TVector<uint32>& VisibleIndices)
    {
        const ImGuiIO& IO = ImGui::GetIO();

        if (IO.KeyShift && SelectionAnchor >= 0)
        {
            // Range over the visible order, so a filtered-out message between the two is not swept in.
            int32 AnchorPos = VisibleIndices.Num();
            int32 ClickPos  = VisibleIndices.Num();

            for (int32 i = 0; i < VisibleIndices.Num(); ++i)
            {
                if (VisibleIndices[i] == (uint32)SelectionAnchor) { AnchorPos = i; }
                if (VisibleIndices[i] == MessageIndex)            { ClickPos  = i; }
            }

            if (AnchorPos < VisibleIndices.Num() && ClickPos < VisibleIndices.Num())
            {
                const int32 Begin = AnchorPos < ClickPos ? AnchorPos : ClickPos;
                const int32 End   = AnchorPos < ClickPos ? ClickPos  : AnchorPos;

                SelectedMessages.Reset();
                for (int32 i = Begin; i <= End; ++i)
                {
                    SelectedMessages.Add(VisibleIndices[i]);
                }
                return;
            }
        }

        if (IO.KeyCtrl)
        {
            if (SelectedMessages.Contains(MessageIndex))
            {
                SelectedMessages.Remove(MessageIndex);
            }
            else
            {
                SelectedMessages.Add(MessageIndex);
            }
        }
        else
        {
            SelectedMessages.Reset();
            SelectedMessages.Add(MessageIndex);
        }

        SelectionAnchor = (int32)MessageIndex;
    }

    void FConsoleLogEditorTool::AppendMessageText(FString& Out, const FConsoleMessage& Message) const
    {
        // Mirrors what the row shows, so a copy matches what was on screen.
        if (Settings.bShowTimestamps)
        {
            Out += "[";
            Out += *Message.Time;
            Out += "] ";
        }

        if (Settings.bShowLogger)
        {
            Out.Append(Message.LoggerName.GetData(), Message.LoggerName.Len());
            Out += ": ";
        }

        Out += *Message.Message;
        Out += "\n";
    }

    void FConsoleLogEditorTool::CopyToClipboard(const TVector<uint32>& VisibleIndices, bool bSelectionOnly) const
    {
        const Logging::FLogQueue& Messages = Logging::GetConsoleLogQueue();

        FString Text;

        // Walk the visible order rather than the set, so copied lines come out in the order shown.
        for (uint32 Index : VisibleIndices)
        {
            if (bSelectionOnly && !SelectedMessages.Contains(Index))
            {
                continue;
            }

            if (Index < static_cast<uint32>(Messages.Num()))
            {
                AppendMessageText(Text, Messages[Index]);
            }
        }

        if (!Text.IsEmpty())
        {
            ImGui::SetClipboardText(*Text);
        }
    }

    const char* FConsoleLogEditorTool::GetLevelIcon(ELogLevel Level) const
    {
        if (!Settings.bShowIcons)
        {
            return "";
        }

        switch (Level)
        {
            case ELogLevel::Error:      return LE_ICON_ALERT_CIRCLE;
            case ELogLevel::Warn:     return LE_ICON_ALERT;
            case ELogLevel::Info:     return LE_ICON_INFORMATION;
            case ELogLevel::Debug:    return LE_ICON_BUG;
            case ELogLevel::Trace:    return LE_ICON_DOTS_HORIZONTAL;
            case ELogLevel::Critical: return LE_ICON_ALERT_OCTAGON;
            default:                      return LE_ICON_INFORMATION;
        }
    }

    const char* FConsoleLogEditorTool::GetLevelLabel(ELogLevel Level) const
    {
        switch (Level)
        {
            case ELogLevel::Error:      return "Error";
            case ELogLevel::Warn:     return "Warning";
            case ELogLevel::Info:     return "Info";
            case ELogLevel::Debug:    return "Debug";
            case ELogLevel::Trace:    return "Trace";
            case ELogLevel::Critical: return "Critical";
            default:                      return "Unknown";
        }
    }
}
