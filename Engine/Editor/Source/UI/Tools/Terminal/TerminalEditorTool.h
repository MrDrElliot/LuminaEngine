#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"
#include "Platform/Process/PlatformPty.h"
#include "TerminalScreen.h"
#include "UI/Tools/EditorTool.h"

namespace Lumina
{
    struct FTerminalSelection
    {
        int32 AnchorRow    = 0;
        int32 AnchorColumn = 0;
        int32 HeadRow      = 0;
        int32 HeadColumn   = 0;

        bool bActive  = false;
        bool bDragging = false;
    };

    struct FTerminalTab
    {
        FString Label;
        FString WorkingDirectory;

        Platform::FPtySessionPtr Session;
        FTerminalScreen          Screen;
        FTerminalSelection       Selection;

        /** Rows scrolled back from the live bottom; zero follows new output. */
        int32 ScrollOffset = 0;

        int32 Columns = 0;
        int32 Rows    = 0;

        bool bExited      = false;
        bool bCloseWanted = false;

        uint32 Id = 0;
    };

    class FTerminalEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FTerminalEditorTool)

        FTerminalEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Terminal", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_CONSOLE_LINE; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

    private:

        struct FSettings
        {
            float FontScale      = 1.0f;
            int32 ScrollbackLimit = 5000;
            bool  bCursorBlink   = true;
            bool  bCopyOnSelect  = false;
        };

        void DrawTerminalWindow(bool bIsFocused);
        void DrawToolbar();
        void DrawTabBar();
        void DrawGrid(FTerminalTab& Tab, bool bIsFocused);
        void DrawCursor(FTerminalTab& Tab, bool bIsFocused, const ImVec2& Origin,
            float CellWidth, float CellHeight, float GlyphSize, int32 TopRow);

        void PumpSessions();

        FTerminalTab* OpenTab(const FString& WorkingDirectory);
        void CloseTab(uint32 Id);
        void RestartTab(FTerminalTab& Tab);

        FTerminalTab* GetActiveTab();

        void SendBytes(FTerminalTab& Tab, const char* Bytes, int32 Count);
        void SendText(FTerminalTab& Tab, FStringView Text);
        void SendKeyboard(FTerminalTab& Tab);
        void PasteClipboard(FTerminalTab& Tab);

        void CopySelection(FTerminalTab& Tab);
        NODISCARD bool IsCellSelected(const FTerminalTab& Tab, int32 Row, int32 Column) const;

        /** Directory a fresh tab starts in, the project root when one is loaded. */
        NODISCARD FString GetStartDirectory() const;

        TVector<TUniquePtr<FTerminalTab>> Tabs;

        uint32 ActiveTabId = 0;
        uint32 NextTabId   = 1;

        FSettings Settings;

        double CursorBlinkTime = 0.0;

        /** Reused across frames so a busy child does not churn the allocator every read. */
        TVector<uint8> ReadBuffer;
    };
}
