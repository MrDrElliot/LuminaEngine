#include "EditorPCH.h"
#include "TerminalEditorTool.h"

#include "Core/Engine/Engine.h"
#include "Core/Math/Scalar.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace
    {
        constexpr int32  GMaxReadBytesPerFrame = 256 * 1024;
        constexpr double GCursorBlinkPeriod    = 1.06;

        struct FSpecialKey
        {
            ImGuiKey    Key;
            const char* Normal;
            const char* Application;
        };

        const FSpecialKey GSpecialKeys[] =
        {
            { ImGuiKey_UpArrow,    "\x1b[A",    "\x1bOA"  },
            { ImGuiKey_DownArrow,  "\x1b[B",    "\x1bOB"  },
            { ImGuiKey_RightArrow, "\x1b[C",    "\x1bOC"  },
            { ImGuiKey_LeftArrow,  "\x1b[D",    "\x1bOD"  },
            { ImGuiKey_Home,       "\x1b[H",    "\x1bOH"  },
            { ImGuiKey_End,        "\x1b[F",    "\x1bOF"  },
            { ImGuiKey_Insert,     "\x1b[2~",   "\x1b[2~" },
            { ImGuiKey_Delete,     "\x1b[3~",   "\x1b[3~" },
            { ImGuiKey_PageUp,     "\x1b[5~",   "\x1b[5~" },
            { ImGuiKey_PageDown,   "\x1b[6~",   "\x1b[6~" },
            { ImGuiKey_F1,         "\x1bOP",    "\x1bOP"  },
            { ImGuiKey_F2,         "\x1bOQ",    "\x1bOQ"  },
            { ImGuiKey_F3,         "\x1bOR",    "\x1bOR"  },
            { ImGuiKey_F4,         "\x1bOS",    "\x1bOS"  },
            { ImGuiKey_F5,         "\x1b[15~",  "\x1b[15~" },
            { ImGuiKey_F6,         "\x1b[17~",  "\x1b[17~" },
            { ImGuiKey_F7,         "\x1b[18~",  "\x1b[18~" },
            { ImGuiKey_F8,         "\x1b[19~",  "\x1b[19~" },
            { ImGuiKey_F9,         "\x1b[20~",  "\x1b[20~" },
            { ImGuiKey_F10,        "\x1b[21~",  "\x1b[21~" },
            { ImGuiKey_F11,        "\x1b[23~",  "\x1b[23~" },
            { ImGuiKey_F12,        "\x1b[24~",  "\x1b[24~" },
        };

        ImU32 ScaleAlpha(ImU32 Color, float Scale)
        {
            const ImU32 Alpha = static_cast<ImU32>(static_cast<float>((Color >> IM_COL32_A_SHIFT) & 0xFF) * Scale);
            return (Color & ~IM_COL32_A_MASK) | (Alpha << IM_COL32_A_SHIFT);
        }

        ImFont* TerminalFont(bool bBold)
        {
            const ImGuiX::Font::EFont Wanted = bBold ? ImGuiX::Font::EFont::MonoBold : ImGuiX::Font::EFont::Mono;
            ImFont* Font = ImGuiX::Font::GFonts[static_cast<int32>(Wanted)];
            return Font != nullptr ? Font : ImGui::GetFont();
        }

        FString ShortenPath(const FString& Path)
        {
            const size_t Slash = Path.find_last_of("/\\");
            return Slash == FString::npos ? Path : Path.substr(Slash + 1);
        }
    }

    void FTerminalEditorTool::OnInitialize()
    {
        CreateToolWindow("Terminal", [this](bool bIsFocused)
        {
            DrawTerminalWindow(bIsFocused);
        },
        ImVec2(0.0f, 0.0f), true);

        if (!Platform::IsPtySupported())
        {
            LOG_WARN("[Terminal] No pseudo-terminal support on this platform; the tool will open empty.");
            return;
        }

        OpenTab(GetStartDirectory());
    }

    void FTerminalEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        Tabs.clear();
    }

    FString FTerminalEditorTool::GetStartDirectory() const
    {
        if (GEngine != nullptr && GEngine->HasLoadedProject())
        {
            const FStringView ProjectPath = GEngine->GetProjectPath();
            return FString(ProjectPath.data(), ProjectPath.size());
        }

        return Paths::GetEngineDirectory();
    }

    FTerminalTab* FTerminalEditorTool::GetActiveTab()
    {
        for (TUniquePtr<FTerminalTab>& Tab : Tabs)
        {
            if (Tab->Id == ActiveTabId)
            {
                return Tab.Get();
            }
        }

        return Tabs.empty() ? nullptr : Tabs.front().Get();
    }

    FTerminalTab* FTerminalEditorTool::OpenTab(const FString& WorkingDirectory)
    {
        TUniquePtr<FTerminalTab> Tab = MakeUnique<FTerminalTab>();

        Tab->Id               = NextTabId++;
        Tab->WorkingDirectory = WorkingDirectory;
        Tab->Columns          = 120;
        Tab->Rows             = 30;
        Tab->Label            = ShortenPath(WorkingDirectory);

        Tab->Screen.Initialize(Tab->Columns, Tab->Rows, Settings.ScrollbackLimit);

        Platform::FPtyLaunchParams Params;
        Params.WorkingDirectory = WorkingDirectory;
        Params.Columns          = static_cast<uint16>(Tab->Columns);
        Params.Rows             = static_cast<uint16>(Tab->Rows);

        Tab->Session = Platform::CreatePtySession(Params);
        if (!Tab->Session)
        {
            LOG_ERROR("[Terminal] Failed to start a shell in '{}'.", WorkingDirectory);
            Tab->bExited = true;
        }

        ActiveTabId = Tab->Id;

        FTerminalTab* Raw = Tab.Get();
        Tabs.push_back(Move(Tab));

        return Raw;
    }

    void FTerminalEditorTool::CloseTab(uint32 Id)
    {
        for (size_t Index = 0; Index < Tabs.size(); ++Index)
        {
            if (Tabs[Index]->Id != Id)
            {
                continue;
            }

            Tabs.erase(Tabs.begin() + static_cast<int32>(Index));

            if (ActiveTabId == Id)
            {
                ActiveTabId = Tabs.empty() ? 0 : Tabs.front()->Id;
            }
            return;
        }
    }

    void FTerminalEditorTool::RestartTab(FTerminalTab& Tab)
    {
        Tab.Session.reset();

        Tab.Screen.Initialize(Math::Max(Tab.Columns, 1), Math::Max(Tab.Rows, 1), Settings.ScrollbackLimit);
        Tab.ScrollOffset = 0;
        Tab.Selection    = FTerminalSelection();

        Platform::FPtyLaunchParams Params;
        Params.WorkingDirectory = Tab.WorkingDirectory;
        Params.Columns          = static_cast<uint16>(Math::Max(Tab.Columns, 1));
        Params.Rows             = static_cast<uint16>(Math::Max(Tab.Rows, 1));

        Tab.Session = Platform::CreatePtySession(Params);
        Tab.bExited = !Tab.Session;
    }

    void FTerminalEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FEditorTool::Update(UpdateContext);

        CursorBlinkTime += UpdateContext.GetDeltaTime();
        while (CursorBlinkTime >= GCursorBlinkPeriod)
        {
            CursorBlinkTime -= GCursorBlinkPeriod;
        }

        PumpSessions();
    }

    void FTerminalEditorTool::PumpSessions()
    {
        for (TUniquePtr<FTerminalTab>& TabPtr : Tabs)
        {
            FTerminalTab& Tab = *TabPtr;
            if (!Tab.Session)
            {
                continue;
            }

            ReadBuffer.clear();
            const int32 ReadCount = Tab.Session->Read(ReadBuffer, GMaxReadBytesPerFrame);

            if (ReadCount > 0)
            {
                Tab.Screen.Write(ReadBuffer.data(), static_cast<int32>(ReadBuffer.size()));

                // New output pins the view to the bottom, matching every other terminal.
                Tab.ScrollOffset = 0;
            }

            FString Reply;
            while (Tab.Screen.ConsumePendingReply(Reply))
            {
                Tab.Session->Write(reinterpret_cast<const uint8*>(Reply.data()), static_cast<int32>(Reply.size()));
            }

            const FString& Title = Tab.Screen.GetTitle();
            if (!Title.empty())
            {
                Tab.Label = Title;
            }

            if (!Tab.bExited && !Tab.Session->IsRunning())
            {
                Tab.bExited = true;

                const FString Notice = Lumina::Format("\r\n\x1b[90m[process exited with code {}]\x1b[0m\r\n",
                    Tab.Session->GetExitCode());

                Tab.Screen.Write(reinterpret_cast<const uint8*>(Notice.data()), static_cast<int32>(Notice.size()));
            }
        }

        for (size_t Index = Tabs.size(); Index > 0; --Index)
        {
            if (Tabs[Index - 1]->bCloseWanted)
            {
                CloseTab(Tabs[Index - 1]->Id);
            }
        }
    }

    void FTerminalEditorTool::SendBytes(FTerminalTab& Tab, const char* Bytes, int32 Count)
    {
        if (!Tab.Session || Tab.bExited || Count <= 0)
        {
            return;
        }

        Tab.Session->Write(reinterpret_cast<const uint8*>(Bytes), Count);

        // Typing pins the view to the bottom and shows the cursor at once, matching every other terminal.
        Tab.ScrollOffset = 0;
        CursorBlinkTime  = 0.0;
    }

    void FTerminalEditorTool::SendText(FTerminalTab& Tab, FStringView Text)
    {
        SendBytes(Tab, Text.data(), static_cast<int32>(Text.size()));
    }

    void FTerminalEditorTool::PasteClipboard(FTerminalTab& Tab)
    {
        const char* Clipboard = ImGui::GetClipboardText();
        if (Clipboard == nullptr || Clipboard[0] == '\0')
        {
            return;
        }

        FString Text(Clipboard);

        // Pasted newlines have to be carriage returns, or the shell sees a stray line feed.
        for (char& Character : Text)
        {
            if (Character == '\n')
            {
                Character = '\r';
            }
        }

        if (Tab.Screen.IsBracketedPaste())
        {
            SendText(Tab, "\x1b[200~");
            SendText(Tab, Text);
            SendText(Tab, "\x1b[201~");
            return;
        }

        SendText(Tab, Text);
    }

    void FTerminalEditorTool::CopySelection(FTerminalTab& Tab)
    {
        if (!Tab.Selection.bActive)
        {
            return;
        }

        const FString Text = Tab.Screen.ExtractRange(
            Tab.Selection.AnchorRow, Tab.Selection.AnchorColumn,
            Tab.Selection.HeadRow, Tab.Selection.HeadColumn);

        if (!Text.empty())
        {
            ImGui::SetClipboardText(Text.c_str());
        }
    }

    bool FTerminalEditorTool::IsCellSelected(const FTerminalTab& Tab, int32 Row, int32 Column) const
    {
        const FTerminalSelection& Selection = Tab.Selection;
        if (!Selection.bActive)
        {
            return false;
        }

        int32 FirstRow    = Selection.AnchorRow;
        int32 FirstColumn = Selection.AnchorColumn;
        int32 LastRow     = Selection.HeadRow;
        int32 LastColumn  = Selection.HeadColumn;

        if (FirstRow > LastRow || (FirstRow == LastRow && FirstColumn > LastColumn))
        {
            const int32 SwapRow    = FirstRow;
            const int32 SwapColumn = FirstColumn;

            FirstRow    = LastRow;
            FirstColumn = LastColumn;
            LastRow     = SwapRow;
            LastColumn  = SwapColumn;
        }

        if (Row < FirstRow || Row > LastRow)
        {
            return false;
        }

        const int32 Low  = (Row == FirstRow) ? FirstColumn : 0;
        const int32 High = (Row == LastRow) ? LastColumn : Tab.Columns - 1;

        return Column >= Low && Column <= High;
    }

    void FTerminalEditorTool::SendKeyboard(FTerminalTab& Tab)
    {
        ImGuiIO& IO = ImGui::GetIO();

        const bool bCtrl  = IO.KeyCtrl;
        const bool bShift = IO.KeyShift;
        const bool bAlt   = IO.KeyAlt;

        if (bCtrl && bShift)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_C, false))
            {
                CopySelection(Tab);
                return;
            }

            if (ImGui::IsKeyPressed(ImGuiKey_V, false))
            {
                PasteClipboard(Tab);
                return;
            }
        }

        const bool bApplication = Tab.Screen.IsApplicationCursorKeys();
        for (const FSpecialKey& Special : GSpecialKeys)
        {
            if (ImGui::IsKeyPressed(Special.Key, true))
            {
                const char* Sequence = bApplication ? Special.Application : Special.Normal;
                SendBytes(Tab, Sequence, static_cast<int32>(strlen(Sequence)));
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Enter, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true))
        {
            SendBytes(Tab, "\r", 1);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true))
        {
            SendBytes(Tab, "\x7f", 1);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Tab, true))
        {
            if (bShift)
            {
                SendBytes(Tab, "\x1b[Z", 3);
            }
            else
            {
                SendBytes(Tab, "\t", 1);
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, true))
        {
            SendBytes(Tab, "\x1b", 1);
        }

        if (bCtrl && !bShift && !bAlt)
        {
            for (int32 Key = ImGuiKey_A; Key <= ImGuiKey_Z; ++Key)
            {
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(Key), true))
                {
                    const char Control = static_cast<char>(1 + (Key - ImGuiKey_A));
                    SendBytes(Tab, &Control, 1);
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
            {
                SendBytes(Tab, "\0", 1);
            }
        }

        // Ctrl already arrived as a key press, but AltGr reports as Ctrl with Alt and must reach the queue.
        if (bCtrl && !bAlt)
        {
            return;
        }

        for (int32 Index = 0; Index < IO.InputQueueCharacters.Size; ++Index)
        {
            const ImWchar Character = IO.InputQueueCharacters[Index];
            if (Character < 0x20 || Character == 0x7F)
            {
                continue;
            }

            FString Encoded;
            if (bAlt)
            {
                Encoded.push_back('\x1b');
            }

            TerminalAppendUtf8(Encoded, static_cast<char32_t>(Character));
            SendText(Tab, Encoded);
        }
    }

    void FTerminalEditorTool::DrawTerminalWindow(bool bIsFocused)
    {
        if (!Platform::IsPtySupported())
        {
            ImGui::TextColored(EditorColors::TextMuted(), "No pseudo-terminal support on this platform.");
            return;
        }

        DrawToolbar();
        DrawTabBar();

        if (FTerminalTab* Tab = GetActiveTab())
        {
            DrawGrid(*Tab, bIsFocused);
        }
        else
        {
            ImGui::TextColored(EditorColors::TextMuted(), "No open terminals. Use the + button to start one.");
        }
    }

    void FTerminalEditorTool::DrawToolbar()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));

        if (ImGui::Button(LE_ICON_PLUS "##NewTerminal"))
        {
            OpenTab(GetStartDirectory());
        }
        ImGuiX::TextTooltip("New terminal");

        FTerminalTab* Tab = GetActiveTab();

        ImGui::SameLine();
        ImGui::BeginDisabled(Tab == nullptr);

        if (ImGui::Button(LE_ICON_RESTART "##RestartTerminal") && Tab != nullptr)
        {
            RestartTab(*Tab);
        }
        ImGuiX::TextTooltip("Restart the shell in this tab");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_BROOM "##ClearTerminal") && Tab != nullptr)
        {
            Tab->Screen.ClearScrollback();
            Tab->ScrollOffset = 0;
        }
        ImGuiX::TextTooltip("Clear scrollback");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CONTENT_COPY "##CopyTerminal") && Tab != nullptr)
        {
            CopySelection(*Tab);
        }
        ImGuiX::TextTooltip("Copy selection (Ctrl+Shift+C)");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CONTENT_PASTE "##PasteTerminal") && Tab != nullptr)
        {
            PasteClipboard(*Tab);
        }
        ImGuiX::TextTooltip("Paste (Ctrl+Shift+V)");

        ImGui::EndDisabled();

        if (Tab != nullptr)
        {
            ImGui::SameLine();
            ImGui::TextColored(EditorColors::TextMuted(), LE_ICON_FOLDER " %s", Tab->WorkingDirectory.c_str());

            ImGui::SameLine();
            ImGui::TextColored(EditorColors::TextMuted(), "  %dx%d", Tab->Columns, Tab->Rows);
        }

        ImGui::PopStyleVar();
        ImGui::Separator();
    }

    void FTerminalEditorTool::DrawTabBar()
    {
        if (Tabs.size() <= 1)
        {
            return;
        }

        if (!ImGui::BeginTabBar("##TerminalTabs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll))
        {
            return;
        }

        for (TUniquePtr<FTerminalTab>& TabPtr : Tabs)
        {
            FTerminalTab& Tab = *TabPtr;

            const FString Label = Lumina::Format("{}###TerminalTab{}", Tab.Label.empty() ? FString("shell") : Tab.Label, Tab.Id);

            bool bOpen = true;
            if (ImGui::BeginTabItem(Label.c_str(), &bOpen))
            {
                ActiveTabId = Tab.Id;
                ImGui::EndTabItem();
            }

            if (!bOpen)
            {
                Tab.bCloseWanted = true;
            }
        }

        ImGui::EndTabBar();
    }

    void FTerminalEditorTool::DrawGrid(FTerminalTab& Tab, bool bIsFocused)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorColors::U32(EditorColors::PanelBg()));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);

        const bool bChildOpen = ImGui::BeginChild("##TerminalGrid", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoNav);

        if (!bChildOpen)
        {
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            return;
        }

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Mono);

        const bool bScaled = Settings.FontScale != 1.0f;
        if (bScaled)
        {
            ImGui::PushFontSize(ImGui::GetFontSize() * Settings.FontScale);
        }

        // Whole-pixel cells keep the glyph, its background and the cursor on the same grid at any column.
        ImFontBaked* Baked = ImGui::GetFontBaked();

        float Advance = Baked != nullptr ? Baked->GetCharAdvance(static_cast<ImWchar>('M')) : 0.0f;
        if (Advance <= 0.0f)
        {
            Advance = ImGui::CalcTextSize("M").x;
        }

        const float GlyphAscent  = Baked != nullptr ? Baked->Ascent  : ImGui::GetFontSize();
        const float GlyphDescent = Baked != nullptr ? -Baked->Descent : 0.0f;

        const float CellWidth  = Math::Max(Math::Ceil(Advance), 1.0f);
        const float CellHeight = Math::Max(Math::Ceil(GlyphAscent + GlyphDescent), 1.0f);

        const ImVec2 Available = ImGui::GetContentRegionAvail();

        const ImVec2 RawOrigin = ImGui::GetCursorScreenPos();
        const ImVec2 Origin(Math::Floor(RawOrigin.x), Math::Floor(RawOrigin.y));

        const int32 NewColumns = Math::Max(static_cast<int32>(Available.x / CellWidth), 1);
        const int32 NewRows    = Math::Max(static_cast<int32>(Available.y / CellHeight), 1);

        if (NewColumns != Tab.Columns || NewRows != Tab.Rows)
        {
            Tab.Columns = NewColumns;
            Tab.Rows    = NewRows;

            Tab.Screen.Resize(NewColumns, NewRows);

            if (Tab.Session)
            {
                Tab.Session->Resize(static_cast<uint16>(NewColumns), static_cast<uint16>(NewRows));
            }
        }

        ImGui::InvisibleButton("##TerminalSurface", ImVec2(Math::Max(Available.x, 1.0f), Math::Max(Available.y, 1.0f)),
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

        const bool bHovered = ImGui::IsItemHovered();

        const int32 MaxScroll  = Tab.Screen.GetScrollbackCount();
        const int32 BottomRow  = Tab.Screen.GetRows() - 1 - Tab.ScrollOffset;
        const int32 TopRow     = BottomRow - (Tab.Rows - 1);

        if (bHovered)
        {
            const float Wheel = ImGui::GetIO().MouseWheel;
            if (Wheel != 0.0f)
            {
                if (Tab.Screen.IsAlternateScreen())
                {
                    // Programs on the alternate screen own scrolling, so the wheel becomes arrow keys.
                    const char* Sequence = Wheel > 0.0f ? "\x1b[A" : "\x1b[B";
                    for (int32 Repeat = 0; Repeat < 3; ++Repeat)
                    {
                        SendBytes(Tab, Sequence, 3);
                    }
                }
                else
                {
                    Tab.ScrollOffset = Math::Clamp(Tab.ScrollOffset + static_cast<int32>(Wheel * 3.0f), 0, MaxScroll);
                }
            }

            const ImVec2 Mouse = ImGui::GetIO().MousePos;
            const int32 HoverColumn = Math::Clamp(static_cast<int32>((Mouse.x - Origin.x) / CellWidth), 0, Tab.Columns - 1);
            const int32 HoverRow    = Math::Clamp(TopRow + static_cast<int32>((Mouse.y - Origin.y) / CellHeight), TopRow, BottomRow);

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                Tab.Selection.AnchorRow    = HoverRow;
                Tab.Selection.AnchorColumn = HoverColumn;
                Tab.Selection.HeadRow      = HoverRow;
                Tab.Selection.HeadColumn   = HoverColumn;
                Tab.Selection.bDragging    = true;
                Tab.Selection.bActive      = false;
            }

            if (Tab.Selection.bDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                Tab.Selection.HeadRow    = HoverRow;
                Tab.Selection.HeadColumn = HoverColumn;

                Tab.Selection.bActive = Tab.Selection.HeadRow != Tab.Selection.AnchorRow
                                     || Tab.Selection.HeadColumn != Tab.Selection.AnchorColumn;
            }

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                Tab.Selection.bDragging = false;

                if (Tab.Selection.bActive && Settings.bCopyOnSelect)
                {
                    CopySelection(Tab);
                }
            }

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                PasteClipboard(Tab);
            }
        }

        const bool bKeyboardOpen = bIsFocused && !ImGui::GetIO().WantTextInput;

        // A child window never takes focus itself, so testing it would gate the keyboard off forever.
        if (bKeyboardOpen)
        {
            ImGui::SetNextFrameWantCaptureKeyboard(true);
            SendKeyboard(Tab);
        }

        ImDrawList* DrawList = ImGui::GetWindowDrawList();

        const ImU32 ThemeForeground = EditorColors::U32(EditorColors::TextPrimary());
        const ImU32 ThemeBackground = EditorColors::U32(EditorColors::PanelBg());
        const ImU32 SelectionColor  = EditorColors::U32(EditorColors::WithAlpha(EditorColors::Accent(), 0.35f));

        Tab.Screen.SetThemeDefaults(ThemeForeground, ThemeBackground);

        const ImU32 DefaultBackground = static_cast<ImU32>(Tab.Screen.GetDefaultBackgroundColor());

        // The child owns the field it paints on, so the whole surface is cleared to its background first.
        DrawList->AddRectFilled(Origin, ImVec2(Origin.x + Available.x, Origin.y + Available.y), DefaultBackground);

        // Text carrying the blink attribute rides the same half-period the cursor does.
        const bool bBlinkPhaseOn = CursorBlinkTime < (GCursorBlinkPeriod * 0.5);

        const float GlyphSize = ImGui::GetFontSize();

        FString Glyph;

        for (int32 ScreenRow = 0; ScreenRow < Tab.Rows; ++ScreenRow)
        {
            const int32 SourceRow = TopRow + ScreenRow;
            const FTerminalLine& Line = Tab.Screen.GetLine(SourceRow);

            const float RowY = Origin.y + static_cast<float>(ScreenRow) * CellHeight;
            const int32 LineWidth = static_cast<int32>(Line.Cells.size());

            int32 Column = 0;
            while (Column < Tab.Columns)
            {
                const bool bInside = Column < LineWidth;
                const FTerminalCell& Cell = bInside ? Line.Cells[static_cast<size_t>(Column)] : FTerminalCell();

                uint32 Foreground = 0;
                uint32 Background = 0;
                Tab.Screen.ResolveCellColors(Cell, bBlinkPhaseOn, Foreground, Background);

                // A run is one span sharing every attribute, so the background and the rules cost one call each.
                const int32 RunStart = Column;

                while (Column < Tab.Columns)
                {
                    const bool bNextInside = Column < LineWidth;
                    const FTerminalCell& Next = bNextInside ? Line.Cells[static_cast<size_t>(Column)] : FTerminalCell();

                    if (Next.Flags != Cell.Flags || Next.Foreground != Cell.Foreground || Next.Background != Cell.Background)
                    {
                        break;
                    }

                    ++Column;
                }

                const float RunX    = Origin.x + static_cast<float>(RunStart) * CellWidth;
                const float RunEndX = Origin.x + static_cast<float>(Column) * CellWidth;

                if (Background != DefaultBackground)
                {
                    DrawList->AddRectFilled(ImVec2(RunX, RowY), ImVec2(RunEndX, RowY + CellHeight), Background);
                }

                ImFont* const RunFont = TerminalFont(Cell.HasFlag(ETerminalCellFlag::Bold));

                // Each glyph is placed on its own cell, so a font whose advance is not the cell width cannot drift.
                if (Foreground != Background)
                {
                    for (int32 GlyphColumn = RunStart; GlyphColumn < Column && GlyphColumn < LineWidth; ++GlyphColumn)
                    {
                        const char32_t Codepoint = Line.Cells[static_cast<size_t>(GlyphColumn)].Codepoint;
                        if (Codepoint == U' ' || Codepoint == 0)
                        {
                            continue;
                        }

                        Glyph.clear();
                        TerminalAppendUtf8(Glyph, Codepoint);

                        const float GlyphX = Origin.x + static_cast<float>(GlyphColumn) * CellWidth;

                        DrawList->AddText(RunFont, GlyphSize, ImVec2(GlyphX, RowY), Foreground,
                            Glyph.c_str(), Glyph.c_str() + Glyph.size());
                    }
                }

                if (Cell.HasFlag(ETerminalCellFlag::Underline))
                {
                    const float UnderlineY = RowY + Math::Floor(GlyphAscent) + 1.0f;
                    DrawList->AddLine(ImVec2(RunX, UnderlineY), ImVec2(RunEndX, UnderlineY), Foreground);
                }

                if (Cell.HasFlag(ETerminalCellFlag::Strike))
                {
                    const float StrikeY = RowY + Math::Floor(GlyphAscent * (2.0f / 3.0f));
                    DrawList->AddLine(ImVec2(RunX, StrikeY), ImVec2(RunEndX, StrikeY), Foreground);
                }
            }

            if (Tab.Selection.bActive)
            {
                for (int32 Selected = 0; Selected < Tab.Columns; ++Selected)
                {
                    if (!IsCellSelected(Tab, SourceRow, Selected))
                    {
                        continue;
                    }

                    const float SelectX = Origin.x + static_cast<float>(Selected) * CellWidth;
                    DrawList->AddRectFilled(ImVec2(SelectX, RowY), ImVec2(SelectX + CellWidth, RowY + CellHeight), SelectionColor);
                }
            }
        }

        DrawCursor(Tab, bIsFocused, Origin, CellWidth, CellHeight, GlyphSize, TopRow);

        if (bScaled)
        {
            ImGui::PopFontSize();
        }

        ImGuiX::Font::PopFont();

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    void FTerminalEditorTool::DrawCursor(FTerminalTab& Tab, bool bIsFocused, const ImVec2& Origin,
        float CellWidth, float CellHeight, float GlyphSize, int32 TopRow)
    {
        if (!Tab.Screen.IsCursorVisible() || Tab.bExited)
        {
            return;
        }

        const int32 CursorRow    = Tab.Screen.GetCursorRow() - TopRow;
        const int32 CursorColumn = Tab.Screen.GetCursorColumn();

        if (CursorRow < 0 || CursorRow >= Tab.Rows || CursorColumn < 0 || CursorColumn >= Tab.Columns)
        {
            return;
        }

        const bool bBlinks  = Settings.bCursorBlink && Tab.Screen.DoesCursorBlink();
        const bool bBlinkOn = !bBlinks || CursorBlinkTime < (GCursorBlinkPeriod * 0.5);

        if (bIsFocused && !bBlinkOn)
        {
            return;
        }

        ImDrawList* DrawList = ImGui::GetWindowDrawList();

        const uint32 Requested = Tab.Screen.GetCursorColor();
        const ImU32 CursorColor = Requested != 0 ? static_cast<ImU32>(Requested)
                                                 : EditorColors::U32(EditorColors::Accent());

        const float CursorX = Origin.x + static_cast<float>(CursorColumn) * CellWidth;
        const float CursorY = Origin.y + static_cast<float>(CursorRow) * CellHeight;

        const ImVec2 Min(CursorX, CursorY);
        const ImVec2 Max(CursorX + CellWidth, CursorY + CellHeight);

        // An unfocused terminal shows the cell outline, which is how every terminal signals it is not listening.
        if (!bIsFocused)
        {
            DrawList->AddRect(Min, Max, ScaleAlpha(CursorColor, 0.7f));
            return;
        }

        const float Thickness = Math::Max(Math::Floor(CellHeight * 0.12f), 1.0f);

        switch (Tab.Screen.GetCursorStyle())
        {
        case ETerminalCursorStyle::Underline:
            DrawList->AddRectFilled(ImVec2(Min.x, Max.y - Thickness), Max, CursorColor);
            return;

        case ETerminalCursorStyle::Bar:
            DrawList->AddRectFilled(Min, ImVec2(Min.x + Thickness, Max.y), CursorColor);
            return;

        case ETerminalCursorStyle::Block:
        default:
            break;
        }

        DrawList->AddRectFilled(Min, Max, CursorColor);

        // The block covers the glyph, so it is redrawn in the background color to stay readable underneath.
        const FTerminalLine& Line = Tab.Screen.GetLine(Tab.Screen.GetCursorRow());
        if (CursorColumn >= static_cast<int32>(Line.Cells.size()))
        {
            return;
        }

        const FTerminalCell& Cell = Line.Cells[static_cast<size_t>(CursorColumn)];
        if (Cell.Codepoint == U' ' || Cell.Codepoint == 0)
        {
            return;
        }

        FString Glyph;
        TerminalAppendUtf8(Glyph, Cell.Codepoint);

        ImFont* const Font = TerminalFont(Cell.HasFlag(ETerminalCellFlag::Bold));

        DrawList->AddText(Font, GlyphSize, Min, static_cast<ImU32>(Tab.Screen.GetDefaultBackgroundColor()),
            Glyph.c_str(), Glyph.c_str() + Glyph.size());
    }

    void FTerminalEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        if (ImGui::BeginMenu(LE_ICON_CONSOLE_LINE " Terminal"))
        {
            if (ImGui::MenuItem("New Terminal"))
            {
                OpenTab(GetStartDirectory());
            }

            if (ImGui::MenuItem("New Terminal at Engine Directory"))
            {
                OpenTab(Paths::GetEngineDirectory());
            }

            ImGui::Separator();

            ImGui::SliderFloat("Font Scale", &Settings.FontScale, 0.7f, 2.0f, "%.1f");

            if (ImGui::SliderInt("Scrollback", &Settings.ScrollbackLimit, 500, 50000))
            {
                // Existing tabs keep their current limit until they restart, which is cheap and predictable.
            }

            ImGui::Checkbox("Blink Cursor", &Settings.bCursorBlink);
            ImGui::Checkbox("Copy On Select", &Settings.bCopyOnSelect);

            ImGui::EndMenu();
        }
    }

    void FTerminalEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Copy / Paste", "Ctrl+Shift+C copies the selection, Ctrl+Shift+V pastes. Right-click also pastes.");
        DrawHelpTextRow("Interrupt", "Ctrl+C sends an interrupt to the running program, as in any terminal.");
        DrawHelpTextRow("Scrollback", "The mouse wheel scrolls history. Programs drawing a full-screen view get arrow keys instead.");
        DrawHelpTextRow("Sizing", "The shell is told the grid size, so wrapping and full-screen programs follow the panel.");
    }
}
