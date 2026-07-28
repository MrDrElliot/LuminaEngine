#pragma once
#include "EditorTool.h"
// Names spdlog::level enumerators in its filter/switch code, so it needs the full enum, not just
// LogMessage.h's forward declaration.
#include "Log/Log.h"

namespace Lumina
{
    enum class EConsoleMessageType : uint8
    {
        Log,
        Command,
        CommandResult,
        Warning,
        Error
    };

    struct FConsoleFilter
    {
        bool bShowTrace = true;
        bool bShowDebug = true;
        bool bShowInfo = true;
        bool bShowWarning = true;
        bool bShowError = true;
        bool bShowCritical = true;
        ImGuiTextFilter TextFilter;

        bool PassesFilter(const FConsoleMessage& Entry) const
        {
            switch (Entry.Level)
            {
                case spdlog::level::trace:    if (!bShowTrace) return false; break;
                case spdlog::level::debug:    if (!bShowDebug) return false; break;
                case spdlog::level::info:     if (!bShowInfo) return false; break;
                case spdlog::level::warn:     if (!bShowWarning) return false; break;
                case spdlog::level::err:      if (!bShowError) return false; break;
                case spdlog::level::critical: if (!bShowCritical) return false; break;
                default: break;
            }

			return TextFilter.PassFilter(Entry.Message.data());
        }
    };

    struct FAutoCompleteCandidate
    {
        FStringView Name;
        FStringView Description;
        FString CurrentValue;
        float MatchScore = 0.0f;

        FAutoCompleteCandidate() = default;
        FAutoCompleteCandidate(FStringView InName, FStringView InDesc = "", const FString& InValue = "", float InScore = 0.0f)
            : Name(InName)
            , Description(InDesc)
            , CurrentValue(InValue)
            , MatchScore(InScore)
        {}
    };
    
    class FConsoleLogEditorTool : public FEditorTool
    {
    public:
        LUMINA_SINGLETON_EDITOR_TOOL(FConsoleLogEditorTool)
    
        FConsoleLogEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Console", nullptr)
            , HistoryIndex(0)
            , bNeedsScrollToBottom(false)
            , FilteredMessageCount(0)
            , bShowAutoComplete(false)
            , AutoCompleteSelectedIndex(0)
            , bShowHistory(false)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_FORMAT_LIST_BULLETED_TYPE; }
        
        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void DrawLogWindow(bool bIsFocused);

    private:

        void ProcessCommand(FStringView Command);
        void AddCommandToHistory(FStringView Command);
        void NavigateHistory(int32 Direction);
        void ClearConsole();
        void ExportLogs(const FString& FilePath);
        void UpdateAutoComplete(FStringView CurrentInput);
        void DrawAutoCompletePopup();
        void DrawHistoryPopup();
        void ApplyCompletion(FStringView Replacement);
        float CalculateMatchScore(FStringView Candidate, FStringView Input);

        static int InputTextCallbackStub(ImGuiInputTextCallbackData* Data);
        int InputTextCallback(ImGuiInputTextCallbackData* Data);

        const char* GetLevelIcon(spdlog::level::level_enum Level) const;
        const char* GetLevelLabel(spdlog::level::level_enum Level) const;
        static ImVec4 GetColorForLevel(spdlog::level::level_enum Level);

        size_t PreviousMessageSize = 0;
        TDeque<FString> CommandHistory;
        char InputBuffer[256] = {};
        FString PendingBufferReplacement;
        bool bPendingBufferReplacement = false;
        bool bFocusInput = true;
        uint64 HistoryIndex;
        bool bNeedsScrollToBottom;
        uint32 FilteredMessageCount;

        struct FConsoleSettings
        {
            bool bAutoScroll = true;
            bool bColorWholeRow = false;
            bool bShowTimestamps = true;
            bool bShowLogger = true;
            bool bShowIcons = true;
            bool bWordWrap = true;
            float FontScale = 1.0f;
            int32 MaxMessageCount = 100;
        } Settings;

        FConsoleFilter Filter;
        
        bool bShowAutoComplete;
        int32 AutoCompleteSelectedIndex;
        TVector<FAutoCompleteCandidate> AutoCompleteCandidates;

        bool bShowHistory;
    };
}