#include "EditorPCH.h"
#include "TerminalScreen.h"

#include "Core/Math/Scalar.h"

namespace Lumina
{
    void TerminalAppendUtf8(FString& Out, char32_t Codepoint)
    {
        if (Codepoint < 0x80)
        {
            Out.push_back(static_cast<char>(Codepoint));
        }
        else if (Codepoint < 0x800)
        {
            Out.push_back(static_cast<char>(0xC0 | (Codepoint >> 6)));
            Out.push_back(static_cast<char>(0x80 | (Codepoint & 0x3F)));
        }
        else if (Codepoint < 0x10000)
        {
            Out.push_back(static_cast<char>(0xE0 | (Codepoint >> 12)));
            Out.push_back(static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3F)));
            Out.push_back(static_cast<char>(0x80 | (Codepoint & 0x3F)));
        }
        else
        {
            Out.push_back(static_cast<char>(0xF0 | (Codepoint >> 18)));
            Out.push_back(static_cast<char>(0x80 | ((Codepoint >> 12) & 0x3F)));
            Out.push_back(static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3F)));
            Out.push_back(static_cast<char>(0x80 | (Codepoint & 0x3F)));
        }
    }

    namespace
    {
        constexpr uint32 PackRGB(uint32 R, uint32 G, uint32 B)
        {
            return 0xFF000000u | (B << 16) | (G << 8) | R;
        }

        // The Campbell scheme, which is what Windows Terminal ships and what most users recognize.
        constexpr uint32 GBasePalette[16] =
        {
            PackRGB( 12,  12,  12), PackRGB(197,  15,  31), PackRGB( 19, 161,  14), PackRGB(193, 156,   0),
            PackRGB(  0,  55, 218), PackRGB(136,  23, 152), PackRGB( 58, 150, 221), PackRGB(204, 204, 204),
            PackRGB(118, 118, 118), PackRGB(231,  72,  86), PackRGB( 22, 198,  12), PackRGB(249, 241, 165),
            PackRGB( 59, 120, 255), PackRGB(180,   0, 158), PackRGB( 97, 214, 214), PackRGB(242, 242, 242),
        };

        uint32 PaletteColor(int32 Index)
        {
            if (Index < 0 || Index > 255)
            {
                return 0;
            }

            if (Index < 16)
            {
                return GBasePalette[Index];
            }

            if (Index < 232)
            {
                const int32 Offset = Index - 16;
                const int32 Steps[6] = { 0, 95, 135, 175, 215, 255 };

                return PackRGB(
                    static_cast<uint32>(Steps[(Offset / 36) % 6]),
                    static_cast<uint32>(Steps[(Offset / 6) % 6]),
                    static_cast<uint32>(Steps[Offset % 6]));
            }

            const uint32 Level = static_cast<uint32>(8 + (Index - 232) * 10);
            return PackRGB(Level, Level, Level);
        }

        uint32 EncodeTrueColor(uint32 R, uint32 G, uint32 B)
        {
            return TerminalTrueColorFlag | (R << 16) | (G << 8) | B;
        }

        uint32 HalveChannels(uint32 Color)
        {
            const uint32 R = ((Color >>  0) & 0xFF) / 2;
            const uint32 G = ((Color >>  8) & 0xFF) / 2;
            const uint32 B = ((Color >> 16) & 0xFF) / 2;
            return 0xFF000000u | (B << 16) | (G << 8) | R;
        }

        uint32 InvertChannels(uint32 Color)
        {
            return (Color ^ 0x00FFFFFFu) | 0xFF000000u;
        }

        bool ParseHexComponent(FStringView Text, uint32& OutValue)
        {
            if (Text.empty() || Text.size() > 4)
            {
                return false;
            }

            uint32 Value = 0;
            for (char Character : Text)
            {
                uint32 Digit = 0;
                if (Character >= '0' && Character <= '9')      { Digit = static_cast<uint32>(Character - '0'); }
                else if (Character >= 'a' && Character <= 'f') { Digit = static_cast<uint32>(Character - 'a') + 10; }
                else if (Character >= 'A' && Character <= 'F') { Digit = static_cast<uint32>(Character - 'A') + 10; }
                else { return false; }

                Value = (Value << 4) | Digit;
            }

            // Every component width scales to eight bits, so 'rgb:f/f/f' and '#ffffff' agree.
            switch (Text.size())
            {
            case 1:  OutValue = Value * 0x11; break;
            case 2:  OutValue = Value;        break;
            case 3:  OutValue = Value >> 4;   break;
            default: OutValue = Value >> 8;   break;
            }

            return true;
        }

        void TrimTrailingSpaces(FString& Text)
        {
            while (!Text.empty() && Text.back() == ' ')
            {
                Text.pop_back();
            }
        }
    }

    void FTerminalScreen::ResetPalette()
    {
        for (int32 Index = 0; Index < 256; ++Index)
        {
            Palette[static_cast<size_t>(Index)] = PaletteColor(Index);
        }
    }

    void FTerminalScreen::SetThemeDefaults(uint32 Foreground, uint32 Background)
    {
        ThemeForeground = Foreground;
        ThemeBackground = Background;
    }

    uint32 FTerminalScreen::GetDefaultForegroundColor() const
    {
        return ForegroundOverride != 0 ? ForegroundOverride : ThemeForeground;
    }

    uint32 FTerminalScreen::GetDefaultBackgroundColor() const
    {
        return BackgroundOverride != 0 ? BackgroundOverride : ThemeBackground;
    }

    uint32 FTerminalScreen::ResolveColor(uint32 Encoded) const
    {
        if ((Encoded & TerminalTrueColorFlag) != 0)
        {
            return PackRGB((Encoded >> 16) & 0xFF, (Encoded >> 8) & 0xFF, Encoded & 0xFF);
        }

        return Encoded < 256 ? Palette[static_cast<size_t>(Encoded)] : 0;
    }

    void FTerminalScreen::ResolveCellColors(const FTerminalCell& Cell, bool bBlinkPhaseOn,
        uint32& OutForeground, uint32& OutBackground) const
    {
        uint32 Foreground = Cell.Foreground;

        // Bold promotes the eight base colors to their bright twins, which is what every terminal does.
        if (Cell.HasFlag(ETerminalCellFlag::Bold) && Foreground < 8)
        {
            Foreground += 8;
        }

        OutForeground = Foreground == TerminalDefaultColor ? GetDefaultForegroundColor() : ResolveColor(Foreground);
        OutBackground = Cell.Background == TerminalDefaultColor ? GetDefaultBackgroundColor() : ResolveColor(Cell.Background);

        if (bScreenReverse)
        {
            OutForeground = OutForeground == GetDefaultForegroundColor() ? GetDefaultBackgroundColor() : InvertChannels(OutForeground);
            OutBackground = OutBackground == GetDefaultBackgroundColor() ? GetDefaultForegroundColor() : InvertChannels(OutBackground);
        }

        if (Cell.HasFlag(ETerminalCellFlag::Dim))
        {
            OutForeground = HalveChannels(OutForeground);
        }

        if (Cell.HasFlag(ETerminalCellFlag::Reverse))
        {
            const uint32 Swap = OutForeground;
            OutForeground = OutBackground;
            OutBackground = Swap;
        }

        if (Cell.HasFlag(ETerminalCellFlag::Invisible) || (Cell.HasFlag(ETerminalCellFlag::Blink) && !bBlinkPhaseOn))
        {
            OutForeground = OutBackground;
        }
    }

    bool FTerminalScreen::ParseColorSpec(FStringView Spec, uint32& OutColor)
    {
        uint32 Components[3] = {};

        if (!Spec.empty() && Spec[0] == '#')
        {
            const FStringView Digits = Spec.substr(1);
            if (Digits.size() % 3 != 0 || Digits.empty() || Digits.size() > 12)
            {
                return false;
            }

            const size_t Width = Digits.size() / 3;
            for (int32 Index = 0; Index < 3; ++Index)
            {
                if (!ParseHexComponent(Digits.substr(static_cast<size_t>(Index) * Width, Width), Components[Index]))
                {
                    return false;
                }
            }

            OutColor = PackRGB(Components[0], Components[1], Components[2]);
            return true;
        }

        if (Spec.size() > 4 && Spec.substr(0, 4) == "rgb:")
        {
            FStringView Rest = Spec.substr(4);

            for (int32 Index = 0; Index < 3; ++Index)
            {
                const size_t Slash = Rest.find('/');
                const FStringView Part = (Index < 2) ? Rest.substr(0, Slash) : Rest;

                if (Index < 2 && Slash == FStringView::npos)
                {
                    return false;
                }

                if (!ParseHexComponent(Part, Components[Index]))
                {
                    return false;
                }

                if (Index < 2)
                {
                    Rest = Rest.substr(Slash + 1);
                }
            }

            OutColor = PackRGB(Components[0], Components[1], Components[2]);
            return true;
        }

        return false;
    }

    void FTerminalScreen::Initialize(int32 InColumns, int32 InRows, int32 InScrollbackLimit)
    {
        ResetPalette();

        Columns         = Math::Max(InColumns, 1);
        Rows            = Math::Max(InRows, 1);
        ScrollbackLimit = Math::Max(InScrollbackLimit, 0);

        Screen.clear();
        Screen.resize(static_cast<size_t>(Rows));

        Scrollback.clear();
        SavedScreen.clear();

        Reset();
    }

    void FTerminalScreen::Reset()
    {
        Pen = FTerminalCell();

        CursorColumn = 0;
        CursorRow    = 0;

        SavedCursorColumn = 0;
        SavedCursorRow    = 0;

        ScrollTop    = 0;
        ScrollBottom = Rows - 1;

        CursorStyle            = ETerminalCursorStyle::Block;
        ForegroundOverride     = 0;
        BackgroundOverride     = 0;
        CursorColorOverride    = 0;
        bCursorStyleBlinks     = true;
        bCursorVisible         = true;
        bAutoWrap              = true;
        bApplicationCursorKeys = false;
        bBracketedPaste        = false;
        bMouseReporting        = false;
        bOriginMode            = false;
        bScreenReverse         = false;
        bWrapPending           = false;

        State = EParseState::Ground;
        bStringEscapePending = false;

        Params.clear();
        CurrentParam    = 0;
        bParamPending   = false;
        bPrivateParams  = false;
        CsiIntermediate = 0;

        Utf8Accumulator = 0;
        Utf8Remaining   = 0;

        OscBuffer.clear();

        for (FTerminalLine& Line : Screen)
        {
            Line.Cells.clear();
            Line.bWrapped = false;
        }
    }

    void FTerminalScreen::ClearScrollback()
    {
        Scrollback.clear();
    }

    void FTerminalScreen::Resize(int32 InColumns, int32 InRows)
    {
        InColumns = Math::Max(InColumns, 1);
        InRows    = Math::Max(InRows, 1);

        if (InColumns == Columns && InRows == Rows)
        {
            return;
        }

        Columns = InColumns;

        // Shrinking pushes the top of the screen into scrollback so the newest output stays put.
        while (InRows < Rows && !Screen.empty())
        {
            if (!bAlternateScreen)
            {
                Scrollback.push_back(Move(Screen.front()));
                while (static_cast<int32>(Scrollback.size()) > ScrollbackLimit)
                {
                    Scrollback.pop_front();
                }
            }

            Screen.erase(Screen.begin());
            --Rows;
            --CursorRow;
        }

        while (InRows > Rows)
        {
            if (!bAlternateScreen && !Scrollback.empty() && CursorRow < Rows)
            {
                Screen.insert(Screen.begin(), Move(Scrollback.back()));
                Scrollback.pop_back();
                ++CursorRow;
            }
            else
            {
                Screen.push_back(FTerminalLine());
            }

            ++Rows;
        }

        ScrollTop    = 0;
        ScrollBottom = Rows - 1;

        ClampCursor();
    }

    const FTerminalLine& FTerminalScreen::GetLine(int32 RowIndex) const
    {
        if (RowIndex >= 0)
        {
            return RowIndex < static_cast<int32>(Screen.size()) ? Screen[static_cast<size_t>(RowIndex)] : BlankLine;
        }

        const int32 Index = static_cast<int32>(Scrollback.size()) + RowIndex;
        return (Index >= 0 && Index < static_cast<int32>(Scrollback.size()))
            ? Scrollback[static_cast<size_t>(Index)]
            : BlankLine;
    }

    bool FTerminalScreen::ConsumePendingReply(FString& Out)
    {
        if (PendingReply.empty())
        {
            return false;
        }

        Out = Move(PendingReply);
        PendingReply.clear();
        return true;
    }

    void FTerminalScreen::PushReply(const char* Text)
    {
        PendingReply.append(Text);
    }

    FTerminalCell FTerminalScreen::MakeBlankCell() const
    {
        FTerminalCell Cell;
        Cell.Codepoint  = U' ';
        Cell.Foreground = Pen.Foreground;
        Cell.Background = Pen.Background;
        Cell.Flags      = 0;
        return Cell;
    }

    FTerminalLine& FTerminalScreen::LiveLine(int32 RowIndex)
    {
        RowIndex = Math::Clamp(RowIndex, 0, Rows - 1);
        return Screen[static_cast<size_t>(RowIndex)];
    }

    void FTerminalScreen::EnsureLineWidth(FTerminalLine& Line, int32 Width) const
    {
        if (static_cast<int32>(Line.Cells.size()) >= Width)
        {
            return;
        }

        FTerminalCell Blank;
        Blank.Codepoint = U' ';
        Line.Cells.resize(static_cast<size_t>(Width), Blank);
    }

    int32 FTerminalScreen::ParamOr(int32 Index, int32 Fallback) const
    {
        if (Index >= static_cast<int32>(Params.size()))
        {
            return Fallback;
        }

        const int32 Value = Params[static_cast<size_t>(Index)];
        return Value == 0 ? Fallback : Value;
    }

    void FTerminalScreen::Write(const uint8* Bytes, int32 Count)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            FeedByte(Bytes[Index]);
        }
    }

    void FTerminalScreen::FeedByte(uint8 Byte)
    {
        if (Utf8Remaining > 0)
        {
            if ((Byte & 0xC0) == 0x80)
            {
                Utf8Accumulator = (Utf8Accumulator << 6) | (Byte & 0x3Fu);
                if (--Utf8Remaining == 0)
                {
                    PutCodepoint(static_cast<char32_t>(Utf8Accumulator));
                }
                return;
            }

            Utf8Remaining = 0;
        }

        switch (State)
        {
        case EParseState::Ground:
            {
                if (Byte == 0x1B)
                {
                    State = EParseState::Escape;
                    return;
                }

                if (Byte < 0x20 || Byte == 0x7F)
                {
                    HandleControl(Byte);
                    return;
                }

                if (Byte < 0x80)
                {
                    PutCodepoint(static_cast<char32_t>(Byte));
                }
                else if ((Byte & 0xE0) == 0xC0)
                {
                    Utf8Accumulator = Byte & 0x1Fu;
                    Utf8Remaining   = 1;
                }
                else if ((Byte & 0xF0) == 0xE0)
                {
                    Utf8Accumulator = Byte & 0x0Fu;
                    Utf8Remaining   = 2;
                }
                else if ((Byte & 0xF8) == 0xF0)
                {
                    Utf8Accumulator = Byte & 0x07u;
                    Utf8Remaining   = 3;
                }
                return;
            }

        case EParseState::Escape:
            HandleEscapeFinal(Byte);
            return;

        case EParseState::EscapeSkipOne:
            State = EParseState::Ground;
            return;

        case EParseState::CsiParam:
        case EParseState::CsiIntermediate:
            {
                if (Byte >= '0' && Byte <= '9')
                {
                    CurrentParam  = Math::Min(CurrentParam * 10 + (Byte - '0'), 65535);
                    bParamPending = true;
                    return;
                }

                if (Byte == ';' || Byte == ':')
                {
                    Params.push_back(bParamPending ? CurrentParam : 0);
                    CurrentParam  = 0;
                    bParamPending = false;
                    return;
                }

                if (Byte == '?' || Byte == '<' || Byte == '=' || Byte == '>')
                {
                    bPrivateParams = true;
                    return;
                }

                if (Byte >= 0x20 && Byte <= 0x2F)
                {
                    CsiIntermediate = Byte;
                    State = EParseState::CsiIntermediate;
                    return;
                }

                if (Byte >= 0x40 && Byte <= 0x7E)
                {
                    if (bParamPending)
                    {
                        Params.push_back(CurrentParam);
                    }
                    HandleCsiFinal(Byte);
                    return;
                }

                if (Byte < 0x20)
                {
                    HandleControl(Byte);
                    return;
                }

                State = EParseState::Ground;
                return;
            }

        case EParseState::OscString:
        case EParseState::StringIgnore:
            {
                if (bStringEscapePending)
                {
                    bStringEscapePending = false;

                    if (State == EParseState::OscString)
                    {
                        HandleOscEnd();
                    }
                    else
                    {
                        State = EParseState::Ground;
                    }
                    return;
                }

                if (Byte == 0x07)
                {
                    if (State == EParseState::OscString)
                    {
                        HandleOscEnd();
                    }
                    else
                    {
                        State = EParseState::Ground;
                    }
                    return;
                }

                if (Byte == 0x1B)
                {
                    bStringEscapePending = true;
                    return;
                }

                if (State == EParseState::OscString && OscBuffer.size() < 1024)
                {
                    OscBuffer.push_back(static_cast<char>(Byte));
                }
                return;
            }
        }
    }

    void FTerminalScreen::HandleControl(uint8 Byte)
    {
        switch (Byte)
        {
        case 0x07: return;
        case 0x08: Backspace(); return;
        case 0x09: HorizontalTab(); return;
        case 0x0A:
        case 0x0B:
        case 0x0C: LineFeed(); return;
        case 0x0D: CarriageReturn(); return;
        default:   return;
        }
    }

    void FTerminalScreen::HandleEscapeFinal(uint8 Byte)
    {
        State = EParseState::Ground;

        switch (Byte)
        {
        case '[':
            Params.clear();
            CurrentParam    = 0;
            bParamPending   = false;
            bPrivateParams  = false;
            CsiIntermediate = 0;
            State = EParseState::CsiParam;
            return;

        case ']':
            OscBuffer.clear();
            bStringEscapePending = false;
            State = EParseState::OscString;
            return;

        case 'P':
        case 'X':
        case '^':
        case '_':
            bStringEscapePending = false;
            State = EParseState::StringIgnore;
            return;

        case '(':
        case ')':
        case '*':
        case '+':
            State = EParseState::EscapeSkipOne;
            return;

        case '7':
            SavedCursorColumn = CursorColumn;
            SavedCursorRow    = CursorRow;
            return;

        case '8':
            SetCursor(SavedCursorColumn, SavedCursorRow);
            return;

        case 'D': LineFeed(); return;
        case 'M': ReverseLineFeed(); return;

        case 'E':
            CarriageReturn();
            LineFeed();
            return;

        case 'c':
            Reset();
            return;

        default:
            return;
        }
    }

    void FTerminalScreen::HandleCsiFinal(uint8 Byte)
    {
        State = EParseState::Ground;

        switch (Byte)
        {
        case 'A': SetCursor(CursorColumn, CursorRow - ParamOr(0, 1)); break;
        case 'B': SetCursor(CursorColumn, CursorRow + ParamOr(0, 1)); break;
        case 'C': SetCursor(CursorColumn + ParamOr(0, 1), CursorRow); break;
        case 'D': SetCursor(CursorColumn - ParamOr(0, 1), CursorRow); break;

        case 'E': SetCursor(0, CursorRow + ParamOr(0, 1)); break;
        case 'F': SetCursor(0, CursorRow - ParamOr(0, 1)); break;

        case 'G':
        case '`':
            SetCursor(ParamOr(0, 1) - 1, CursorRow);
            break;

        case 'd':
            SetCursor(CursorColumn, ParamOr(0, 1) - 1);
            break;

        case 'H':
        case 'f':
            SetCursor(ParamOr(1, 1) - 1, ParamOr(0, 1) - 1);
            break;

        case 'J': EraseInDisplay(Params.empty() ? 0 : Params[0]); break;
        case 'K': EraseInLine(Params.empty() ? 0 : Params[0]); break;

        case 'L': InsertLines(ParamOr(0, 1)); break;
        case 'M': DeleteLines(ParamOr(0, 1)); break;

        case '@': InsertCells(ParamOr(0, 1)); break;
        case 'P': DeleteCells(ParamOr(0, 1)); break;
        case 'X': EraseCells(ParamOr(0, 1)); break;

        case 'S': ScrollRegionUp(ParamOr(0, 1)); break;
        case 'T': ScrollRegionDown(ParamOr(0, 1)); break;

        case 'm': HandleSgr(); break;

        case 'h': HandleMode(true); break;
        case 'l': HandleMode(false); break;

        case 'r':
            {
                const int32 Top    = ParamOr(0, 1) - 1;
                const int32 Bottom = Params.size() >= 2 ? ParamOr(1, Rows) - 1 : Rows - 1;

                if (Top < Bottom)
                {
                    ScrollTop    = Math::Clamp(Top, 0, Rows - 1);
                    ScrollBottom = Math::Clamp(Bottom, 0, Rows - 1);
                    SetCursor(0, bOriginMode ? ScrollTop : 0);
                }
                break;
            }

        case 's':
            SavedCursorColumn = CursorColumn;
            SavedCursorRow    = CursorRow;
            break;

        case 'u':
            SetCursor(SavedCursorColumn, SavedCursorRow);
            break;

        case 'n':
            {
                if (!Params.empty() && Params[0] == 6)
                {
                    char Report[32] = {};
                    snprintf(Report, sizeof(Report), "\x1b[%d;%dR", CursorRow + 1, CursorColumn + 1);
                    PushReply(Report);
                }
                else if (!Params.empty() && Params[0] == 5)
                {
                    PushReply("\x1b[0n");
                }
                break;
            }

        case 'c':
            // Identify as a VT102, which is what most programs probe for.
            PushReply("\x1b[?6c");
            break;

        case 'q':
            if (CsiIntermediate == ' ')
            {
                const int32 Style = Params.empty() ? 0 : Params[0];

                switch (Style)
                {
                case 0:
                case 1:  CursorStyle = ETerminalCursorStyle::Block;     bCursorStyleBlinks = true;  break;
                case 2:  CursorStyle = ETerminalCursorStyle::Block;     bCursorStyleBlinks = false; break;
                case 3:  CursorStyle = ETerminalCursorStyle::Underline; bCursorStyleBlinks = true;  break;
                case 4:  CursorStyle = ETerminalCursorStyle::Underline; bCursorStyleBlinks = false; break;
                case 5:  CursorStyle = ETerminalCursorStyle::Bar;       bCursorStyleBlinks = true;  break;
                case 6:  CursorStyle = ETerminalCursorStyle::Bar;       bCursorStyleBlinks = false; break;
                default: break;
                }
            }
            break;

        default:
            break;
        }
    }

    void FTerminalScreen::HandleMode(bool bSet)
    {
        for (int32 Value : Params)
        {
            if (!bPrivateParams)
            {
                continue;
            }

            switch (Value)
            {
            case 1:    bApplicationCursorKeys = bSet; break;
            case 7:    bAutoWrap = bSet; break;
            case 5:    bScreenReverse = bSet; break;
            case 12:   bCursorStyleBlinks = bSet; break;
            case 25:   bCursorVisible = bSet; break;
            case 1000:
            case 1002:
            case 1003:
            case 1006: bMouseReporting = bSet; break;
            case 2004: bBracketedPaste = bSet; break;

            case 6:
                bOriginMode = bSet;
                SetCursor(0, bSet ? ScrollTop : 0);
                break;

            case 47:
            case 1047:
            case 1049:
                SwitchScreen(bSet);
                break;

            default:
                break;
            }
        }
    }

    void FTerminalScreen::HandleSgr()
    {
        if (Params.empty())
        {
            Pen = FTerminalCell();
            return;
        }

        for (size_t Index = 0; Index < Params.size(); ++Index)
        {
            const int32 Code = Params[Index];

            if (Code == 0)
            {
                Pen = FTerminalCell();
            }
            else if (Code == 1)  { Pen.Flags |= static_cast<uint8>(ETerminalCellFlag::Bold); }
            else if (Code == 2)  { Pen.Flags |= static_cast<uint8>(ETerminalCellFlag::Dim); }
            else if (Code == 3)  { Pen.Flags |= static_cast<uint8>(ETerminalCellFlag::Italic); }
            else if (Code == 4)  { Pen.Flags |= static_cast<uint8>(ETerminalCellFlag::Underline); }
            else if (Code == 5 || Code == 6) { Pen.Flags |= static_cast<uint8>(ETerminalCellFlag::Blink); }
            else if (Code == 7)  { Pen.Flags |= static_cast<uint8>(ETerminalCellFlag::Reverse); }
            else if (Code == 8)  { Pen.Flags |= static_cast<uint8>(ETerminalCellFlag::Invisible); }
            else if (Code == 9)  { Pen.Flags |= static_cast<uint8>(ETerminalCellFlag::Strike); }
            else if (Code == 22) { Pen.Flags &= ~static_cast<uint8>(static_cast<uint8>(ETerminalCellFlag::Bold) | static_cast<uint8>(ETerminalCellFlag::Dim)); }
            else if (Code == 23) { Pen.Flags &= ~static_cast<uint8>(ETerminalCellFlag::Italic); }
            else if (Code == 24) { Pen.Flags &= ~static_cast<uint8>(ETerminalCellFlag::Underline); }
            else if (Code == 25) { Pen.Flags &= ~static_cast<uint8>(ETerminalCellFlag::Blink); }
            else if (Code == 27) { Pen.Flags &= ~static_cast<uint8>(ETerminalCellFlag::Reverse); }
            else if (Code == 28) { Pen.Flags &= ~static_cast<uint8>(ETerminalCellFlag::Invisible); }
            else if (Code == 29) { Pen.Flags &= ~static_cast<uint8>(ETerminalCellFlag::Strike); }
            else if (Code >= 30 && Code <= 37)   { Pen.Foreground = static_cast<uint32>(Code - 30); }
            else if (Code == 39)                 { Pen.Foreground = TerminalDefaultColor; }
            else if (Code >= 40 && Code <= 47)   { Pen.Background = static_cast<uint32>(Code - 40); }
            else if (Code == 49)                 { Pen.Background = TerminalDefaultColor; }
            else if (Code >= 90 && Code <= 97)   { Pen.Foreground = static_cast<uint32>(Code - 90 + 8); }
            else if (Code >= 100 && Code <= 107) { Pen.Background = static_cast<uint32>(Code - 100 + 8); }
            else if (Code == 38 || Code == 48)
            {
                uint32* Target = (Code == 38) ? &Pen.Foreground : &Pen.Background;

                if (Index + 1 < Params.size() && Params[Index + 1] == 5)
                {
                    if (Index + 2 < Params.size())
                    {
                        *Target = static_cast<uint32>(Math::Clamp(Params[Index + 2], 0, 255));
                    }
                    Index += 2;
                }
                else if (Index + 1 < Params.size() && Params[Index + 1] == 2)
                {
                    if (Index + 4 < Params.size())
                    {
                        *Target = EncodeTrueColor(
                            static_cast<uint32>(Math::Clamp(Params[Index + 2], 0, 255)),
                            static_cast<uint32>(Math::Clamp(Params[Index + 3], 0, 255)),
                            static_cast<uint32>(Math::Clamp(Params[Index + 4], 0, 255)));
                    }
                    Index += 4;
                }
            }
        }
    }

    void FTerminalScreen::HandleOscEnd()
    {
        State = EParseState::Ground;
        bStringEscapePending = false;

        const size_t Separator = OscBuffer.find(';');
        const FStringView Whole(OscBuffer);

        const FStringView CommandText = Whole.substr(0, Separator == FString::npos ? Whole.size() : Separator);
        const FStringView Arguments   = Separator == FString::npos ? FStringView() : Whole.substr(Separator + 1);

        int32 Command = -1;
        if (!CommandText.empty() && CommandText.size() <= 3)
        {
            Command = 0;
            for (char Character : CommandText)
            {
                if (Character < '0' || Character > '9')
                {
                    Command = -1;
                    break;
                }
                Command = Command * 10 + (Character - '0');
            }
        }

        // Both the window title and the icon title land in the tab label.
        if (Command == 0 || Command == 1 || Command == 2)
        {
            Title.assign(Arguments.data(), Arguments.size());
        }
        else if (Command >= 0)
        {
            HandleOscColor(Command, Arguments);
        }

        OscBuffer.clear();
    }

    void FTerminalScreen::HandleOscColor(int32 Command, FStringView Arguments)
    {
        const auto SplitNext = [](FStringView& Text) -> FStringView
        {
            const size_t Separator = Text.find(';');
            if (Separator == FStringView::npos)
            {
                const FStringView Whole = Text;
                Text = FStringView();
                return Whole;
            }

            const FStringView Head = Text.substr(0, Separator);
            Text = Text.substr(Separator + 1);
            return Head;
        };

        const auto ParseIndex = [](FStringView Text, int32& OutIndex) -> bool
        {
            if (Text.empty() || Text.size() > 3)
            {
                return false;
            }

            int32 Value = 0;
            for (char Character : Text)
            {
                if (Character < '0' || Character > '9')
                {
                    return false;
                }
                Value = Value * 10 + (Character - '0');
            }

            OutIndex = Value;
            return Value >= 0 && Value <= 255;
        };

        switch (Command)
        {
        case 4:
            {
                FStringView Rest = Arguments;
                while (!Rest.empty())
                {
                    int32 Index = 0;
                    if (!ParseIndex(SplitNext(Rest), Index))
                    {
                        return;
                    }

                    uint32 Color = 0;
                    if (!ParseColorSpec(SplitNext(Rest), Color))
                    {
                        return;
                    }

                    Palette[static_cast<size_t>(Index)] = Color;
                }
                return;
            }

        case 104:
            {
                if (Arguments.empty())
                {
                    ResetPalette();
                    return;
                }

                FStringView Rest = Arguments;
                while (!Rest.empty())
                {
                    int32 Index = 0;
                    if (!ParseIndex(SplitNext(Rest), Index))
                    {
                        return;
                    }

                    Palette[static_cast<size_t>(Index)] = PaletteColor(Index);
                }
                return;
            }

        case 10:
        case 11:
        case 12:
            {
                uint32 Color = 0;
                if (!ParseColorSpec(Arguments, Color))
                {
                    return;
                }

                if (Command == 10)      { ForegroundOverride  = Color; }
                else if (Command == 11) { BackgroundOverride  = Color; }
                else                    { CursorColorOverride = Color; }
                return;
            }

        case 110: ForegroundOverride  = 0; return;
        case 111: BackgroundOverride  = 0; return;
        case 112: CursorColorOverride = 0; return;

        default:
            return;
        }
    }

    void FTerminalScreen::PutCodepoint(char32_t Codepoint)
    {
        if (bWrapPending && bAutoWrap)
        {
            LiveLine(CursorRow).bWrapped = true;
            CarriageReturn();
            LineFeed();
        }

        bWrapPending = false;

        FTerminalLine& Line = LiveLine(CursorRow);
        EnsureLineWidth(Line, CursorColumn + 1);

        FTerminalCell& Cell = Line.Cells[static_cast<size_t>(CursorColumn)];
        Cell.Codepoint  = Codepoint;
        Cell.Foreground = Pen.Foreground;
        Cell.Background = Pen.Background;
        Cell.Flags      = Pen.Flags;

        if (CursorColumn + 1 >= Columns)
        {
            bWrapPending = bAutoWrap;
        }
        else
        {
            ++CursorColumn;
        }
    }

    void FTerminalScreen::LineFeed()
    {
        bWrapPending = false;

        if (CursorRow == ScrollBottom)
        {
            ScrollRegionUp(1);
        }
        else if (CursorRow < Rows - 1)
        {
            ++CursorRow;
        }
    }

    void FTerminalScreen::ReverseLineFeed()
    {
        bWrapPending = false;

        if (CursorRow == ScrollTop)
        {
            ScrollRegionDown(1);
        }
        else if (CursorRow > 0)
        {
            --CursorRow;
        }
    }

    void FTerminalScreen::CarriageReturn()
    {
        CursorColumn = 0;
        bWrapPending = false;
    }

    void FTerminalScreen::Backspace()
    {
        bWrapPending = false;

        if (CursorColumn > 0)
        {
            --CursorColumn;
        }
    }

    void FTerminalScreen::HorizontalTab()
    {
        bWrapPending = false;

        const int32 Next = ((CursorColumn / 8) + 1) * 8;
        CursorColumn = Math::Min(Next, Columns - 1);
    }

    void FTerminalScreen::ScrollRegionUp(int32 Count)
    {
        Count = Math::Clamp(Count, 0, ScrollBottom - ScrollTop + 1);

        for (int32 Index = 0; Index < Count; ++Index)
        {
            // Only a full-height region on the primary screen contributes to history.
            if (!bAlternateScreen && ScrollTop == 0)
            {
                Scrollback.push_back(Move(Screen[static_cast<size_t>(ScrollTop)]));
                while (static_cast<int32>(Scrollback.size()) > ScrollbackLimit)
                {
                    Scrollback.pop_front();
                }
            }

            Screen.erase(Screen.begin() + ScrollTop);
            Screen.insert(Screen.begin() + ScrollBottom, FTerminalLine());
        }
    }

    void FTerminalScreen::ScrollRegionDown(int32 Count)
    {
        Count = Math::Clamp(Count, 0, ScrollBottom - ScrollTop + 1);

        for (int32 Index = 0; Index < Count; ++Index)
        {
            Screen.erase(Screen.begin() + ScrollBottom);
            Screen.insert(Screen.begin() + ScrollTop, FTerminalLine());
        }
    }

    void FTerminalScreen::EraseInDisplay(int32 Mode)
    {
        if (Mode == 3)
        {
            Scrollback.clear();
            return;
        }

        if (Mode == 2)
        {
            for (FTerminalLine& Line : Screen)
            {
                Line.Cells.clear();
                Line.bWrapped = false;
            }
            return;
        }

        if (Mode == 0)
        {
            EraseInLine(0);
            for (int32 Row = CursorRow + 1; Row < Rows; ++Row)
            {
                Screen[static_cast<size_t>(Row)].Cells.clear();
                Screen[static_cast<size_t>(Row)].bWrapped = false;
            }
            return;
        }

        EraseInLine(1);
        for (int32 Row = 0; Row < CursorRow; ++Row)
        {
            Screen[static_cast<size_t>(Row)].Cells.clear();
            Screen[static_cast<size_t>(Row)].bWrapped = false;
        }
    }

    void FTerminalScreen::EraseInLine(int32 Mode)
    {
        FTerminalLine& Line = LiveLine(CursorRow);

        if (Mode == 0)
        {
            if (CursorColumn < static_cast<int32>(Line.Cells.size()))
            {
                Line.Cells.resize(static_cast<size_t>(CursorColumn));
            }

            // A background set by the pen has to be painted rather than dropped.
            if (Pen.Background != TerminalDefaultColor)
            {
                EnsureLineWidth(Line, Columns);
                for (int32 Column = CursorColumn; Column < Columns; ++Column)
                {
                    Line.Cells[static_cast<size_t>(Column)] = MakeBlankCell();
                }
            }

            Line.bWrapped = false;
            return;
        }

        const int32 Last = (Mode == 1) ? Math::Min(CursorColumn, Columns - 1) : Columns - 1;

        EnsureLineWidth(Line, Last + 1);
        for (int32 Column = 0; Column <= Last; ++Column)
        {
            Line.Cells[static_cast<size_t>(Column)] = MakeBlankCell();
        }

        if (Mode == 2)
        {
            Line.bWrapped = false;
        }
    }

    void FTerminalScreen::InsertLines(int32 Count)
    {
        if (CursorRow < ScrollTop || CursorRow > ScrollBottom)
        {
            return;
        }

        Count = Math::Clamp(Count, 0, ScrollBottom - CursorRow + 1);

        for (int32 Index = 0; Index < Count; ++Index)
        {
            Screen.erase(Screen.begin() + ScrollBottom);
            Screen.insert(Screen.begin() + CursorRow, FTerminalLine());
        }
    }

    void FTerminalScreen::DeleteLines(int32 Count)
    {
        if (CursorRow < ScrollTop || CursorRow > ScrollBottom)
        {
            return;
        }

        Count = Math::Clamp(Count, 0, ScrollBottom - CursorRow + 1);

        for (int32 Index = 0; Index < Count; ++Index)
        {
            Screen.erase(Screen.begin() + CursorRow);
            Screen.insert(Screen.begin() + ScrollBottom, FTerminalLine());
        }
    }

    void FTerminalScreen::InsertCells(int32 Count)
    {
        FTerminalLine& Line = LiveLine(CursorRow);
        EnsureLineWidth(Line, Columns);

        Count = Math::Clamp(Count, 0, Columns - CursorColumn);

        for (int32 Index = 0; Index < Count; ++Index)
        {
            Line.Cells.pop_back();
            Line.Cells.insert(Line.Cells.begin() + CursorColumn, MakeBlankCell());
        }
    }

    void FTerminalScreen::DeleteCells(int32 Count)
    {
        FTerminalLine& Line = LiveLine(CursorRow);
        EnsureLineWidth(Line, Columns);

        Count = Math::Clamp(Count, 0, Columns - CursorColumn);

        for (int32 Index = 0; Index < Count; ++Index)
        {
            Line.Cells.erase(Line.Cells.begin() + CursorColumn);
            Line.Cells.push_back(MakeBlankCell());
        }
    }

    void FTerminalScreen::EraseCells(int32 Count)
    {
        FTerminalLine& Line = LiveLine(CursorRow);

        const int32 Last = Math::Min(CursorColumn + Math::Max(Count, 1), Columns);
        EnsureLineWidth(Line, Last);

        for (int32 Column = CursorColumn; Column < Last; ++Column)
        {
            Line.Cells[static_cast<size_t>(Column)] = MakeBlankCell();
        }
    }

    void FTerminalScreen::SetCursor(int32 Column, int32 Row)
    {
        CursorColumn = Column;
        CursorRow    = bOriginMode ? Row + ScrollTop : Row;

        bWrapPending = false;
        ClampCursor();
    }

    void FTerminalScreen::ClampCursor()
    {
        CursorColumn = Math::Clamp(CursorColumn, 0, Math::Max(Columns - 1, 0));
        CursorRow    = Math::Clamp(CursorRow, 0, Math::Max(Rows - 1, 0));
    }

    void FTerminalScreen::SwitchScreen(bool bToAlternate)
    {
        if (bToAlternate == bAlternateScreen)
        {
            return;
        }

        bAlternateScreen = bToAlternate;

        if (bToAlternate)
        {
            SavedScreen = Move(Screen);

            Screen.clear();
            Screen.resize(static_cast<size_t>(Rows));

            SavedCursorColumn = CursorColumn;
            SavedCursorRow    = CursorRow;

            SetCursor(0, 0);
        }
        else
        {
            Screen = Move(SavedScreen);
            SavedScreen.clear();

            // A resize while the alternate screen was up leaves the saved one the wrong height.
            Screen.resize(static_cast<size_t>(Rows));

            SetCursor(SavedCursorColumn, SavedCursorRow);
        }

        ScrollTop    = 0;
        ScrollBottom = Rows - 1;
    }

    FString FTerminalScreen::ExtractText(int32 FirstRow, int32 LastRow) const
    {
        FString Out;

        for (int32 Row = FirstRow; Row <= LastRow; ++Row)
        {
            const FTerminalLine& Line = GetLine(Row);

            FString LineText;
            for (const FTerminalCell& Cell : Line.Cells)
            {
                TerminalAppendUtf8(LineText, Cell.Codepoint);
            }

            TrimTrailingSpaces(LineText);
            Out.append(LineText);

            if (Row < LastRow && !Line.bWrapped)
            {
                Out.push_back('\n');
            }
        }

        return Out;
    }

    FString FTerminalScreen::ExtractRange(int32 StartRow, int32 StartColumn, int32 EndRow, int32 EndColumn) const
    {
        if (StartRow > EndRow || (StartRow == EndRow && StartColumn > EndColumn))
        {
            const int32 SwapRow    = StartRow;
            const int32 SwapColumn = StartColumn;

            StartRow    = EndRow;
            StartColumn = EndColumn;
            EndRow      = SwapRow;
            EndColumn   = SwapColumn;
        }

        FString Out;

        for (int32 Row = StartRow; Row <= EndRow; ++Row)
        {
            const FTerminalLine& Line = GetLine(Row);
            const int32 Width = static_cast<int32>(Line.Cells.size());

            const int32 First = (Row == StartRow) ? Math::Max(StartColumn, 0) : 0;
            const int32 Last  = (Row == EndRow) ? Math::Min(EndColumn, Width - 1) : Width - 1;

            FString LineText;
            for (int32 Column = First; Column <= Last; ++Column)
            {
                TerminalAppendUtf8(LineText, Line.Cells[static_cast<size_t>(Column)].Codepoint);
            }

            TrimTrailingSpaces(LineText);
            Out.append(LineText);

            if (Row < EndRow && !Line.bWrapped)
            {
                Out.push_back('\n');
            }
        }

        return Out;
    }
}
