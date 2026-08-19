#pragma once
#include "Containers/Deque.h"
#include "EditorTool.h"
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
                case ELogLevel::Trace:    if (!bShowTrace) return false; break;
                case ELogLevel::Debug:    if (!bShowDebug) return false; break;
                case ELogLevel::Info:     if (!bShowInfo) return false; break;
                case ELogLevel::Warn:     if (!bShowWarning) return false; break;
                case ELogLevel::Error:      if (!bShowError) return false; break;
                case ELogLevel::Critical: if (!bShowCritical) return false; break;
                default: break;
            }

			// Phrase match, unlike the rest of the editor's search boxes: OR-on-space would make
			// "failed to load" match every line holding "to".
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
        const char* GetTitlebarIcon() const override { return LE_ICON_CONSOLE; }
        
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

        // Selection is keyed on the index into the log queue, so it survives the filter changing
        // under it. Anchor is a queue index too; the shift-range resolves it against the visible list.
        void HandleRowClick(uint32 MessageIndex, const TVector<uint32>& VisibleIndices);
        void CopyToClipboard(const TVector<uint32>& VisibleIndices, bool bSelectionOnly) const;
        void AppendMessageText(FString& Out, const FConsoleMessage& Message) const;

        const char* GetLevelIcon(ELogLevel Level) const;
        const char* GetLevelLabel(ELogLevel Level) const;
        static ImVec4 GetColorForLevel(ELogLevel Level);

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

        THashSet<uint32> SelectedMessages;
        int32 SelectionAnchor = -1;

        bool bShowAutoComplete;
        int32 AutoCompleteSelectedIndex;
        TVector<FAutoCompleteCandidate> AutoCompleteCandidates;

        bool bShowHistory;
    };
}
