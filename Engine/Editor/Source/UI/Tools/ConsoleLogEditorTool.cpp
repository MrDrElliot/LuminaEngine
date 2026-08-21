#include "ConsoleLogEditorTool.h"

#include <utility>

#include "Core/Console/ConsoleVariable.h"
#include "Log/LogMessage.h"
#include "Log/Log.h"

namespace Lumina
{
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
            "Up/Down with empty input cycles command history.");
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
        ImGui::Text("Messages: %u / %zu", FilteredMessageCount, PreviousMessageSize);
    }

    void FConsoleLogEditorTool::DrawLogWindow(bool bIsFocused)
    {
        const float InputHeight = ImGui::GetFrameHeightWithSpacing() * 1.2f;
        
        ImGui::SetNextItemWidth(-1);
        
        Filter.TextFilter.Draw("##LogFilter");

        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.04f, 0.05f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.82f, 0.78f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));

        const float LogHeight = ImGui::GetContentRegionAvail().y - InputHeight;
        ImGui::BeginChild("##LogMessages", ImVec2(0, LogHeight), true,
            ImGuiWindowFlags_HorizontalScrollbar);

        // Sticking keys off the scroll position, since the ring's size stops growing once saturated.
        const bool bWasPinnedToBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

        const Logging::FLogQueue& Messages = Logging::GetConsoleLogQueue();
        PreviousMessageSize = Messages.size();

        if (Settings.FontScale != 1.0f)
        {
            ImGui::PushFontSize(ImGui::GetFontSize() * Settings.FontScale);
        }

        // Cheap, since it holds indices only and allocates nothing beyond vector growth
        TVector<uint32> VisibleIndices;
        VisibleIndices.reserve(Messages.size());
        for (size_t i = 0; i < Messages.size(); ++i)
        {
            if (Filter.PassesFilter(Messages[i]))
            {
                VisibleIndices.push_back((uint32)i);
            }
        }
        FilteredMessageCount = (uint32)VisibleIndices.size();

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
                ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 1.0f), "[%s]", Message.Time.c_str());
                ImGui::SameLine(0, 6);
            }

            if (Settings.bShowIcons)
            {
                ImGui::TextColored(Color, "%s", GetLevelIcon(Message.Level));
                ImGui::SameLine(0, 4);
            }

            if (Settings.bShowLogger)
            {
                ImGui::TextColored(ImVec4(0.55f, 0.70f, 0.90f, 1.0f), "%s:", Message.LoggerName.data());
                ImGui::SameLine(0, 6);
            }

            if (Settings.bWordWrap)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Color);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(Message.Message.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextColored(Color, "%s", Message.Message.c_str());
            }

            const ImVec2 RowMax(RowRight, ImGui::GetCursorScreenPos().y);

            if (SelectedMessages.find(Index) != SelectedMessages.end())
            {
                RowDrawList->ChannelsSetCurrent(0);
                RowDrawList->AddRectFilled(RowMin, RowMax, IM_COL32(38, 79, 120, 255));
                RowDrawList->ChannelsSetCurrent(1);
            }

            if (bLogHovered && ImGui::IsMouseHoveringRect(RowMin, RowMax))
            {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    HandleRowClick(Index, VisibleIndices);
                }
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                    && SelectedMessages.find(Index) == SelectedMessages.end())
                {
                    // Right-clicking outside the selection retargets it, matching every list view.
                    HandleRowClick(Index, VisibleIndices);
                }
            }
        };

        if (bUseClipper)
        {
            ImGuiListClipper Clipper;
            Clipper.Begin((int)VisibleIndices.size());
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
                CopyToClipboard(VisibleIndices, !SelectedMessages.empty());
            }

            if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))
            {
                SelectedMessages.clear();
                for (uint32 Visible : VisibleIndices)
                {
                    SelectedMessages.insert(Visible);
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                SelectedMessages.clear();
                SelectionAnchor = -1;
            }
        }

        if (ImGui::BeginPopupContextWindow("##LogContext"))
        {
            const size_t SelectedCount = SelectedMessages.size();

            if (ImGui::MenuItem("Copy", "Ctrl+C", false, SelectedCount > 0))
            {
                CopyToClipboard(VisibleIndices, true);
            }

            if (ImGui::MenuItem("Copy All Visible"))
            {
                CopyToClipboard(VisibleIndices, false);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Select All", "Ctrl+A"))
            {
                SelectedMessages.clear();
                for (uint32 Visible : VisibleIndices)
                {
                    SelectedMessages.insert(Visible);
                }
            }

            if (ImGui::MenuItem("Clear Selection", "Esc", false, SelectedCount > 0))
            {
                SelectedMessages.clear();
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

        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.04f, 0.04f, 0.05f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.85f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));

        ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.45f, 1.0f), ">");
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

        if (bExecuteCommand)
        {
            FStringView Command(InputBuffer);
            if (!Command.empty())
            {
                ProcessCommand(Command);
                AddCommandToHistory(Command);
                InputBuffer[0] = '\0';
                bNeedsScrollToBottom = true;
                bShowAutoComplete = false;
                AutoCompleteCandidates.clear();
            }
            bFocusInput = true;
        }

        if (bInputActive && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            if (bShowAutoComplete)
            {
                bShowAutoComplete = false;
                AutoCompleteCandidates.clear();
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

        if (bShowAutoComplete && !AutoCompleteCandidates.empty())
        {
            DrawAutoCompletePopup();
        }

        if (bShowHistory)
        {
            DrawHistoryPopup();
        }
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
            case ImGuiInputTextFlags_CallbackAlways:
            {
                if (bPendingBufferReplacement)
                {
                    Data->DeleteChars(0, Data->BufTextLen);
                    Data->InsertChars(0, PendingBufferReplacement.c_str());
                    Data->CursorPos = Data->BufTextLen;
                    Data->SelectionStart = Data->SelectionEnd = Data->CursorPos;
                    bPendingBufferReplacement = false;
                    PendingBufferReplacement.clear();
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
                if (bShowAutoComplete && !AutoCompleteCandidates.empty())
                {
                    int32 Index = AutoCompleteSelectedIndex;
                    if (Index < 0 || Index >= (int32)AutoCompleteCandidates.size())
                    {
                        Index = 0;
                    }

                    FStringView Replacement = AutoCompleteCandidates[Index].Name;
                    Data->DeleteChars(0, Data->BufTextLen);
                    Data->InsertChars(0, Replacement.data(), Replacement.data() + Replacement.size());
                    Data->CursorPos = Data->BufTextLen;
                    bShowAutoComplete = false;
                    AutoCompleteCandidates.clear();
                }
                break;
            }
            case ImGuiInputTextFlags_CallbackHistory:
            {
                if (bShowAutoComplete && !AutoCompleteCandidates.empty())
                {
                    if (Data->EventKey == ImGuiKey_DownArrow)
                    {
                        AutoCompleteSelectedIndex = (AutoCompleteSelectedIndex + 1) % (int32)AutoCompleteCandidates.size();
                    }
                    else if (Data->EventKey == ImGuiKey_UpArrow)
                    {
                        AutoCompleteSelectedIndex--;
                        if (AutoCompleteSelectedIndex < 0)
                        {
                            AutoCompleteSelectedIndex = (int32)AutoCompleteCandidates.size() - 1;
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
                        const FString& Replacement = (HistoryIndex < CommandHistory.size())
                            ? CommandHistory[HistoryIndex]
                            : FString();
                        Data->DeleteChars(0, Data->BufTextLen);
                        if (!Replacement.empty())
                        {
                            Data->InsertChars(0, Replacement.c_str());
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
        PendingBufferReplacement.assign(Replacement.data(), Replacement.size());
        bPendingBufferReplacement = true;
        bShowAutoComplete = false;
        AutoCompleteCandidates.clear();
        bFocusInput = true;
    }

    ImVec4 FConsoleLogEditorTool::GetColorForLevel(ELogLevel Level)
    {
        switch (Level)
        {
            case ELogLevel::Error:      return ImVec4(0.95f, 0.25f, 0.25f, 1.0f);
            case ELogLevel::Warn:     return ImVec4(1.00f, 0.70f, 0.20f, 1.0f);
            case ELogLevel::Info:     return ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
            case ELogLevel::Debug:    return ImVec4(0.45f, 0.80f, 1.00f, 1.0f);
            case ELogLevel::Trace:    return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
            case ELogLevel::Critical: return ImVec4(1.00f, 0.10f, 0.10f, 1.0f);
            default:                      return ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
        }
    }

    void FConsoleLogEditorTool::ProcessCommand(FStringView Command)
    {
        LOG_INFO("> {}", Command);

        size_t Index = Command.find_first_of(' ');
        FStringView Name = (Index == FString::npos) ? Command : Command.substr(0, Index);
        FStringView ValueString = (Index == FString::npos) ? FStringView{} : Command.substr(Index + 1);

        FConsoleRegistry& Registry = FConsoleRegistry::Get();

        if (Registry.FindCommand(Name))
        {
            if (Index != FString::npos)
            {
                LOG_WARN("'{}' is a command and takes no arguments; ignoring '{}'", Name, ValueString);
            }
            Registry.ExecuteCommand(Name);
            return;
        }

        if (Registry.Find(Name))
        {
            if (Index == FString::npos)
            {
                TOptional<FString> Value = Registry.GetValueAsString(Name);
                LOG_INFO("{} = {}", Name, Value.has_value() ? *Value : FString("<unprintable>"));
                return;
            }

            if (Registry.SetValueFromString(Name, ValueString))
            {
                LOG_INFO("{} New Value: {}", Name, ValueString);
            }
            else
            {
                LOG_WARN("Failed to set '{}' to '{}'", Name, ValueString);
            }
            return;
        }

        LOG_WARN("Unknown console command or variable: '{}'", Name);
    }

    void FConsoleLogEditorTool::AddCommandToHistory(FStringView Command)
    {
        if (!CommandHistory.empty() && CommandHistory.back() == Command)
        {
            HistoryIndex = CommandHistory.size();
            return;
        }

        CommandHistory.push_back(FString(Command));

        constexpr size_t MaxHistory = 100;
        if (CommandHistory.size() > MaxHistory)
        {
            CommandHistory.pop_front();
        }
        
        HistoryIndex = CommandHistory.size();
    }

    void FConsoleLogEditorTool::NavigateHistory(int32 Direction)
    {
        if (CommandHistory.empty())
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
            if (HistoryIndex < CommandHistory.size())
            {
                HistoryIndex++;
            }
        }
    }

    void FConsoleLogEditorTool::ClearConsole()
    {
        Logging::ClearLogQueue();
        PreviousMessageSize = 0;
        FilteredMessageCount = 0;
        bNeedsScrollToBottom = false;
    }

    void FConsoleLogEditorTool::ExportLogs(const FString& FilePath)
    {
        LOG_INFO("Exporting logs to: {}", FilePath);
    }

    void FConsoleLogEditorTool::UpdateAutoComplete(FStringView CurrentInput)
    {
        AutoCompleteCandidates.clear();
        AutoCompleteSelectedIndex = 0;

        if (CurrentInput.empty())
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

                if (Value.has_value())
                {
                    AutoCompleteCandidates.emplace_back(Name, Var.Hint, Value.value(), Score);
                }
            }
        }

        for (const auto& [Name, Cmd] : Registry.GetAllCommands())
        {
            float Score = CalculateMatchScore(Name, CurrentInput);
            if (Score > 0.0f)
            {
                AutoCompleteCandidates.emplace_back(Name, Cmd.Hint, FString(), Score);
            }
        }

        Algo::Sort(AutoCompleteCandidates.begin(), AutoCompleteCandidates.end(), [](const FAutoCompleteCandidate& A, const FAutoCompleteCandidate& B)
        {
            return A.MatchScore > B.MatchScore;
        });

        if (AutoCompleteCandidates.size() > 10)
        {
            AutoCompleteCandidates.resize(10);
        }

        bShowAutoComplete = !AutoCompleteCandidates.empty();
    }

    void FConsoleLogEditorTool::DrawAutoCompletePopup()
    {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y - 5), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetItemRectSize().x, 0));

        ImGuiWindowFlags PopupFlags = 
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_AlwaysAutoResize;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.35f, 1.0f));

        if (ImGui::Begin("##AutoComplete", nullptr, PopupFlags))
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Auto-Complete (%zu matches)", AutoCompleteCandidates.size());
            ImGui::Separator();

            for (int32 i = 0; i < (int32)AutoCompleteCandidates.size(); ++i)
            {
                const FAutoCompleteCandidate& Candidate = AutoCompleteCandidates[i];
                
                bool bIsSelected = (i == AutoCompleteSelectedIndex);
                
                if (bIsSelected)
                {
                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.5f, 0.8f, 0.8f));
                }

                ImGui::PushID(i);
                
                if (ImGui::Selectable("##Candidate", bIsSelected, 0, ImVec2(0, 0)))
                {
                    ApplyCompletion(Candidate.Name);
                }

                ImGui::SameLine(0, 4);

                ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "%s", Candidate.Name.data());
                
                if (!Candidate.CurrentValue.empty())
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "= %s", Candidate.CurrentValue.c_str());
                }

                if (!Candidate.Description.empty())
                {
                    ImGui::Indent(20.0f);
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", Candidate.Description.data());
                    ImGui::Unindent(20.0f);
                }

                ImGui::PopID();

                if (bIsSelected)
                {
                    ImGui::PopStyleColor();
                }
            }

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), LE_ICON_ARROW_UP_DOWN " Navigate  " LE_ICON_KEYBOARD_TAB " Accept  " LE_ICON_KEYBOARD_ESC " Cancel");
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    void FConsoleLogEditorTool::DrawHistoryPopup()
    {
        ImGui::SetNextWindowPos(ImVec2(
            ImGui::GetItemRectMin().x,
            ImGui::GetItemRectMin().y - 5
        ), ImGuiCond_Always, ImVec2(0.0f, 1.0f));

        ImGui::SetNextWindowSize(ImVec2(ImGui::GetItemRectSize().x, 300));

        ImGuiWindowFlags PopupFlags = 
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 3));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.35f, 1.0f));

        if (ImGui::Begin("##CommandHistory", nullptr, PopupFlags))
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), LE_ICON_HISTORY " Command History (%zu)", CommandHistory.size());
            
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear"))
            {
                CommandHistory.clear();
                HistoryIndex = 0;
            }

            ImGui::Separator();

            ImGui::BeginChild("##HistoryList", ImVec2(0, 0), false);

            if (CommandHistory.empty())
            {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No command history");
            }
            else
            {
                for (int32 i = (int32)CommandHistory.size() - 1; i >= 0; --i)
                {
                    const FString& Cmd = CommandHistory[i];
                    
                    ImGui::PushID(i);
                    
                    bool bIsSelected = (i == (int32)HistoryIndex);
                    
                    if (ImGui::Selectable(Cmd.c_str(), bIsSelected))
                    {
                        ApplyCompletion(Cmd);
                        bShowHistory = false;
                    }


                    ImGui::PopID();
                }
            }

            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        // Click outside to close
        if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
        {
            bShowHistory = false;
        }
    }
    
    float FConsoleLogEditorTool::CalculateMatchScore(FStringView Candidate, FStringView Input)
    {
        if (Candidate.empty() || Input.empty())
        {
            return 0.0f;
        }
        
        if (Candidate.starts_with(Input))
        {
            return 1.0f;
        }

        size_t Pos = Candidate.find(Input);
        if (Pos != FString::npos)
        {
            return 0.7f - static_cast<float>(Pos) / static_cast<float>(Input.size()) * 0.2f;
        }

        size_t CandidatePos = 0;
        size_t MatchedChars = 0;
        
        for (size_t i = 0; i < Input.size() && CandidatePos < Candidate.size(); ++i)
        {
            char InputChar = std::tolower(Input[i]);
            while (CandidatePos < Candidate.size())
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

        if (MatchedChars == Input.size())
        {
            return 0.5f * static_cast<float>(MatchedChars) / static_cast<float>(Candidate.size());
        }

        return 0.0f;
    }
    
    void FConsoleLogEditorTool::HandleRowClick(uint32 MessageIndex, const TVector<uint32>& VisibleIndices)
    {
        const ImGuiIO& IO = ImGui::GetIO();

        if (IO.KeyShift && SelectionAnchor >= 0)
        {
            // Range over the visible order, so a filtered-out message between the two is not swept in.
            size_t AnchorPos = VisibleIndices.size();
            size_t ClickPos  = VisibleIndices.size();

            for (size_t i = 0; i < VisibleIndices.size(); ++i)
            {
                if (VisibleIndices[i] == (uint32)SelectionAnchor) { AnchorPos = i; }
                if (VisibleIndices[i] == MessageIndex)            { ClickPos  = i; }
            }

            if (AnchorPos < VisibleIndices.size() && ClickPos < VisibleIndices.size())
            {
                const size_t Begin = AnchorPos < ClickPos ? AnchorPos : ClickPos;
                const size_t End   = AnchorPos < ClickPos ? ClickPos  : AnchorPos;

                SelectedMessages.clear();
                for (size_t i = Begin; i <= End; ++i)
                {
                    SelectedMessages.insert(VisibleIndices[i]);
                }
                return;
            }
        }

        if (IO.KeyCtrl)
        {
            if (SelectedMessages.find(MessageIndex) != SelectedMessages.end())
            {
                SelectedMessages.erase(MessageIndex);
            }
            else
            {
                SelectedMessages.insert(MessageIndex);
            }
        }
        else
        {
            SelectedMessages.clear();
            SelectedMessages.insert(MessageIndex);
        }

        SelectionAnchor = (int32)MessageIndex;
    }

    void FConsoleLogEditorTool::AppendMessageText(FString& Out, const FConsoleMessage& Message) const
    {
        // Mirrors what the row shows, so a copy matches what was on screen.
        if (Settings.bShowTimestamps)
        {
            Out += "[";
            Out += Message.Time.c_str();
            Out += "] ";
        }

        if (Settings.bShowLogger)
        {
            Out.append(Message.LoggerName.data(), Message.LoggerName.size());
            Out += ": ";
        }

        Out += Message.Message.c_str();
        Out += "\n";
    }

    void FConsoleLogEditorTool::CopyToClipboard(const TVector<uint32>& VisibleIndices, bool bSelectionOnly) const
    {
        const Logging::FLogQueue& Messages = Logging::GetConsoleLogQueue();

        FString Text;

        // Walk the visible order rather than the set, so copied lines come out in the order shown.
        for (uint32 Index : VisibleIndices)
        {
            if (bSelectionOnly && SelectedMessages.find(Index) == SelectedMessages.end())
            {
                continue;
            }

            if (Index < Messages.size())
            {
                AppendMessageText(Text, Messages[Index]);
            }
        }

        if (!Text.empty())
        {
            ImGui::SetClipboardText(Text.c_str());
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
