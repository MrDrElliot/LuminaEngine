#pragma once

#include "Containers/Deque.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    void TerminalAppendUtf8(FString& Out, char32_t Codepoint);

    // A cell color is a palette index, a truecolor value behind the flag, or the theme default.
    constexpr uint32 TerminalDefaultColor  = 0xFFFFFFFFu;
    constexpr uint32 TerminalTrueColorFlag = 0x80000000u;

    enum class ETerminalCursorStyle : uint8
    {
        Block,
        Underline,
        Bar,
    };

    enum class ETerminalCellFlag : uint8
    {
        None      = 0,
        Bold      = 1 << 0,
        Dim       = 1 << 1,
        Italic    = 1 << 2,
        Underline = 1 << 3,
        Reverse   = 1 << 4,
        Strike    = 1 << 5,
        Invisible = 1 << 6,
        Blink     = 1 << 7,
    };

    struct FTerminalCell
    {
        char32_t Codepoint = U' ';

        uint32 Foreground = TerminalDefaultColor;
        uint32 Background = TerminalDefaultColor;

        uint8 Flags = 0;

        NODISCARD bool HasFlag(ETerminalCellFlag Flag) const { return (Flags & static_cast<uint8>(Flag)) != 0; }
    };

    struct FTerminalLine
    {
        TVector<FTerminalCell> Cells;

        /** Set when the line ran past the right edge, so a copy can rejoin it with the next one. */
        bool bWrapped = false;
    };

    /** Cell grid plus the VT parser that drives it, with no dependency on how it gets drawn. */
    class FTerminalScreen
    {
    public:

        void Initialize(int32 InColumns, int32 InRows, int32 InScrollbackLimit);

        void Resize(int32 InColumns, int32 InRows);

        void Write(const uint8* Bytes, int32 Count);

        void Reset();

        void ClearScrollback();

        NODISCARD int32 GetColumns() const { return Columns; }
        NODISCARD int32 GetRows() const { return Rows; }
        NODISCARD int32 GetScrollbackCount() const { return static_cast<int32>(Scrollback.size()); }

        /** Negative indexes reach into scrollback, zero through Rows-1 address the live screen. */
        NODISCARD const FTerminalLine& GetLine(int32 RowIndex) const;

        NODISCARD int32 GetCursorColumn() const { return CursorColumn; }
        NODISCARD int32 GetCursorRow() const { return CursorRow; }
        NODISCARD bool IsCursorVisible() const { return bCursorVisible; }
        NODISCARD ETerminalCursorStyle GetCursorStyle() const { return CursorStyle; }
        NODISCARD bool DoesCursorBlink() const { return bCursorStyleBlinks; }

        NODISCARD bool IsAlternateScreen() const { return bAlternateScreen; }
        NODISCARD bool IsApplicationCursorKeys() const { return bApplicationCursorKeys; }
        NODISCARD bool IsBracketedPaste() const { return bBracketedPaste; }
        NODISCARD bool IsMouseReportingEnabled() const { return bMouseReporting; }

        NODISCARD const FString& GetTitle() const { return Title; }

        // The theme colors a cell falls back to, which OSC 10 and OSC 11 can still override.
        void SetThemeDefaults(uint32 Foreground, uint32 Background);

        // Draw-ready 0xAABBGGRR for one cell, folding in bold, faint, reverse and blink in VT order.
        void ResolveCellColors(const FTerminalCell& Cell, bool bBlinkPhaseOn,
            uint32& OutForeground, uint32& OutBackground) const;

        // Zero when the child has not asked for a cursor color through OSC 12.
        NODISCARD uint32 GetCursorColor() const { return CursorColorOverride; }

        NODISCARD uint32 GetDefaultForegroundColor() const;
        NODISCARD uint32 GetDefaultBackgroundColor() const;

        /** Drains anything the terminal owes the child, such as a cursor position report. */
        bool ConsumePendingReply(FString& Out);

        /** Plain text for the inclusive row range, used by copy and by anything reading the buffer. */
        NODISCARD FString ExtractText(int32 FirstRow, int32 LastRow) const;

        NODISCARD FString ExtractRange(int32 StartRow, int32 StartColumn, int32 EndRow, int32 EndColumn) const;

    private:

        enum class EParseState : uint8
        {
            Ground,
            Escape,
            EscapeSkipOne,
            CsiParam,
            CsiIntermediate,
            OscString,
            StringIgnore,
        };

        void FeedByte(uint8 Byte);
        void PutCodepoint(char32_t Codepoint);

        void HandleControl(uint8 Byte);
        void HandleEscapeFinal(uint8 Byte);
        void HandleCsiFinal(uint8 Byte);
        void HandleOscEnd();
        void HandleSgr();
        void HandleMode(bool bSet);

        void LineFeed();
        void ReverseLineFeed();
        void CarriageReturn();
        void Backspace();
        void HorizontalTab();

        void ScrollRegionUp(int32 Count);
        void ScrollRegionDown(int32 Count);

        void EraseInDisplay(int32 Mode);
        void EraseInLine(int32 Mode);
        void InsertLines(int32 Count);
        void DeleteLines(int32 Count);
        void InsertCells(int32 Count);
        void DeleteCells(int32 Count);
        void EraseCells(int32 Count);

        void SetCursor(int32 Column, int32 Row);
        void ClampCursor();

        void SwitchScreen(bool bToAlternate);

        FTerminalLine& LiveLine(int32 RowIndex);
        void EnsureLineWidth(FTerminalLine& Line, int32 Width) const;

        FTerminalCell MakeBlankCell() const;

        NODISCARD int32 ParamOr(int32 Index, int32 Fallback) const;

        void PushReply(const char* Text);

        void ResetPalette();

        NODISCARD uint32 ResolveColor(uint32 Encoded) const;

        // Parses an XParseColor spec, either '#RRGGBB' or 'rgb:RR/GG/BB' at any component width.
        NODISCARD static bool ParseColorSpec(FStringView Spec, uint32& OutColor);

        void HandleOscColor(int32 Command, FStringView Arguments);

        int32 Columns = 80;
        int32 Rows    = 24;
        int32 ScrollbackLimit = 5000;

        TVector<FTerminalLine> Screen;
        TVector<FTerminalLine> SavedScreen;
        TDeque<FTerminalLine>  Scrollback;

        int32 CursorColumn = 0;
        int32 CursorRow    = 0;

        int32 SavedCursorColumn = 0;
        int32 SavedCursorRow    = 0;

        int32 ScrollTop    = 0;
        int32 ScrollBottom = 0;

        FTerminalCell Pen;

        ETerminalCursorStyle CursorStyle = ETerminalCursorStyle::Block;

        bool bCursorStyleBlinks     = true;
        bool bCursorVisible         = true;
        bool bAutoWrap              = true;
        bool bAlternateScreen       = false;
        bool bApplicationCursorKeys = false;
        bool bBracketedPaste        = false;
        bool bMouseReporting        = false;
        bool bOriginMode            = false;
        bool bScreenReverse         = false;

        /** Set once the cursor sits past the last column, so the wrap happens on the next glyph. */
        bool bWrapPending = false;

        EParseState State = EParseState::Ground;

        /** An ESC seen inside a string, which ends it when the next byte is the backslash of an ST. */
        bool bStringEscapePending = false;

        TVector<int32> Params;

        // The last 0x20 to 0x2F byte of the CSI sequence, which is what separates DECSCUSR from SU.
        uint8 CsiIntermediate = 0;

        bool  bPrivateParams  = false;
        bool  bParamPending   = false;
        int32 CurrentParam    = 0;

        FString OscBuffer;
        FString Title;
        FString PendingReply;

        uint32 Utf8Accumulator = 0;
        int32  Utf8Remaining   = 0;

        uint32 Palette[256] = {};

        uint32 ThemeForeground = 0xFFCCCCCCu;
        uint32 ThemeBackground = 0xFF0C0C0Cu;

        uint32 ForegroundOverride = 0;
        uint32 BackgroundOverride = 0;
        uint32 CursorColorOverride = 0;

        FTerminalLine BlankLine;
    };
}
