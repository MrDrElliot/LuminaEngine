#include "RuntimePCH.h"
#include "DevConsole.h"

#if USING(WITH_DEV_UI)

#include "Containers/Algorithm.h"
#include "Core/Console/ConsoleVariable.h"
#include "Log/Log.h"
#include "Log/LogMessage.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"

#include <cctype>
#include <imgui.h>

namespace Lumina
{
    namespace
    {
        constexpr float kConsoleHeightFraction = 0.4f;
        constexpr int32 kMaxSuggestions = 8;

        // See-through enough to keep the game readable behind the log.
        constexpr float kConsoleBackgroundAlpha = 0.7f;

        // The grave key opens and closes the overlay, so its characters never reach the command line.
        bool IsToggleCharacter(ImWchar Character)
        {
            return Character == '`' || Character == '~';
        }

        ImVec4 LevelColor(ELogLevel Level)
        {
            switch (Level)
            {
            case ELogLevel::Warn:     return ImVec4(1.00f, 0.80f, 0.30f, 1.0f);
            case ELogLevel::Error:    return ImVec4(1.00f, 0.40f, 0.35f, 1.0f);
            case ELogLevel::Critical: return ImVec4(1.00f, 0.15f, 0.15f, 1.0f);
            case ELogLevel::Debug:    return ImVec4(0.45f, 0.80f, 1.00f, 1.0f);
            case ELogLevel::Trace:    return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
            default:                  return ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
            }
        }

        bool StartsWithIgnoreCase(FStringView Text, FStringView Prefix)
        {
            if (Prefix.Len() > Text.Len())
            {
                return false;
            }
            for (int32 Index = 0; Index < Prefix.Len(); ++Index)
            {
                if (std::tolower((unsigned char)Text[Index]) != std::tolower((unsigned char)Prefix[Index]))
                {
                    return false;
                }
            }
            return true;
        }

        FStringView FirstWord(const char* Line)
        {
            const FStringView View(Line);
            const int32 Space = View.FindFirstOf(' ');
            return Space == Constants::kIndexNone ? View : View.Mid(0, Space);
        }
    }

    void FDevConsole::Draw(bool bFocusInput, bool& bKeepScriptWindowsVisible)
    {
        const ImGuiViewport* Viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(Viewport->WorkPos, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(Viewport->WorkSize.x, Viewport->WorkSize.y * kConsoleHeightFraction), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(kConsoleBackgroundAlpha);

        if (ImGui::Begin(LE_ICON_CONSOLE " Console###DevConsole", nullptr, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::Checkbox("Keep script windows visible when closed", &bKeepScriptWindowsVisible);
            ImGui::SameLine();
            ImGui::TextDisabled("` to close");
            ImGui::Separator();

            DrawLog();
            DrawInput(bFocusInput);
            DrawSuggestions();
        }
        ImGui::End();
    }

    void FDevConsole::DrawLog()
    {
        const float InputHeight = ImGui::GetFrameHeightWithSpacing() * 1.5f;
        if (ImGui::BeginChild("##DevConsoleLog", ImVec2(0.0f, -InputHeight), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
        {
            const bool bWasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

            const Logging::FLogQueue& Messages = Logging::GetConsoleLogQueue();
            ImGuiListClipper Clipper;
            Clipper.Begin(Messages.Num());
            while (Clipper.Step())
            {
                for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
                {
                    const FConsoleMessage& Message = Messages[Row];
                    ImGui::TextDisabled("%s", *Message.Time);
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(Message.Level));
                    ImGui::TextUnformatted(*Message.Message);
                    ImGui::PopStyleColor();
                }
            }

            if (bScrollToBottom || bWasAtBottom)
            {
                ImGui::SetScrollHereY(1.0f);
            }
            bScrollToBottom = false;
        }
        ImGui::EndChild();
    }

    void FDevConsole::DrawInput(bool bFocusInput)
    {
        constexpr ImGuiInputTextFlags Flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory
            | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackCharFilter;

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (bFocusInput)
        {
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::InputTextWithHint("##DevConsoleInput", "Command or variable, Tab completes", InputBuffer, kInputCapacity, Flags, &FDevConsole::InputCallback, this))
        {
            Submit();
            ImGui::SetKeyboardFocusHere(-1);
        }
    }

    void FDevConsole::DrawSuggestions()
    {
        const FStringView Word = FirstWord(InputBuffer);
        if (Word.IsEmpty() || FStringView(InputBuffer).Find(' ') != Constants::kIndexNone || !ImGui::IsItemActive())
        {
            return;
        }

        TVector<FString> Matches;
        GatherMatches(Word, Matches);
        if (Matches.IsEmpty())
        {
            return;
        }

        // Drawn as a tooltip under the command line, so it overlaps the log rather than resizing the window.
        const ImVec2 Anchor(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y);
        ImGui::SetNextWindowPos(Anchor);
        if (ImGui::BeginTooltip())
        {
            FConsoleRegistry& Registry = FConsoleRegistry::Get();
            for (int32 Index = 0; Index < Matches.Num() && Index < kMaxSuggestions; ++Index)
            {
                const FString& Name = Matches[Index];
                if (Registry.FindCommand(Name) != nullptr)
                {
                    ImGui::Text("%s", *Name);
                    ImGui::SameLine();
                    ImGui::TextDisabled("command");
                    continue;
                }
                const TOptional<FString> Value = Registry.GetValueAsString(Name);
                ImGui::Text("%s", *Name);
                ImGui::SameLine();
                ImGui::TextDisabled("= %s", Value.IsSet() ? *(*Value) : "?");
            }
            if (Matches.Num() > kMaxSuggestions)
            {
                ImGui::TextDisabled("%d more", Matches.Num() - kMaxSuggestions);
            }
            ImGui::EndTooltip();
        }
    }

    void FDevConsole::Submit()
    {
        const FStringView Line(InputBuffer);
        if (!Line.IsEmpty() && (History.IsEmpty() || History.Last() != Line))
        {
            History.Emplace(Line.GetData(), Line.Len());
        }
        HistoryPosition = -1;

        FConsoleRegistry::Get().ExecuteLine(Line, true);
        InputBuffer[0] = '\0';
        bScrollToBottom = true;
    }

    int FDevConsole::InputCallback(ImGuiInputTextCallbackData* Data)
    {
        return static_cast<FDevConsole*>(Data->UserData)->HandleInputCallback(Data);
    }

    int FDevConsole::HandleInputCallback(ImGuiInputTextCallbackData* Data)
    {
        if (Data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
        {
            return IsToggleCharacter(Data->EventChar) ? 1 : 0;
        }

        if (Data->EventFlag == ImGuiInputTextFlags_CallbackHistory && !History.IsEmpty())
        {
            const int32 Last = History.Num() - 1;
            if (Data->EventKey == ImGuiKey_UpArrow)
            {
                HistoryPosition = HistoryPosition < 0 ? Last : Math::Max(HistoryPosition - 1, 0);
            }
            else if (Data->EventKey == ImGuiKey_DownArrow && HistoryPosition >= 0)
            {
                HistoryPosition = HistoryPosition < Last ? HistoryPosition + 1 : -1;
            }

            Data->DeleteChars(0, Data->BufTextLen);
            if (HistoryPosition >= 0)
            {
                Data->InsertChars(0, *History[HistoryPosition]);
            }
            return 0;
        }

        if (Data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
        {
            const FStringView Typed(Data->Buf, Data->BufTextLen);
            if (Typed.IsEmpty() || Typed.Find(' ') != Constants::kIndexNone)
            {
                return 0;
            }

            TVector<FString> Matches;
            GatherMatches(Typed, Matches);
            if (Matches.IsEmpty())
            {
                return 0;
            }

            // Extends to the longest prefix every match shares, and a single match gets a space ready for its value.
            int32 Common = Matches[0].Len();
            for (const FString& Match : Matches)
            {
                int32 Shared = 0;
                while (Shared < Common && Shared < Match.Len()
                    && std::tolower((unsigned char)Match[Shared]) == std::tolower((unsigned char)Matches[0][Shared]))
                {
                    ++Shared;
                }
                Common = Shared;
            }

            FString Completed = Matches[0].Mid(0, Common);
            if (Matches.Num() == 1)
            {
                Completed += " ";
            }
            Data->DeleteChars(0, Data->BufTextLen);
            Data->InsertChars(0, *Completed);
        }
        return 0;
    }

    void FDevConsole::GatherMatches(FStringView Prefix, TVector<FString>& Out) const
    {
        Out.Reset();
        FConsoleRegistry& Registry = FConsoleRegistry::Get();
        for (const auto& [Name, Variable] : Registry.GetAll())
        {
            if (StartsWithIgnoreCase(Name, Prefix))
            {
                Out.Emplace(Name.GetData(), Name.Len());
            }
        }
        for (const auto& [Name, Command] : Registry.GetAllCommands())
        {
            if (StartsWithIgnoreCase(Name, Prefix))
            {
                Out.Emplace(Name.GetData(), Name.Len());
            }
        }
        Algo::Sort(Out);
    }
}

#endif
