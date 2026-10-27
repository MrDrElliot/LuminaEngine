#include "RmlLanguage.h"

#include <cmath>
#include <cstdlib>
#include <imgui.h>
#include "RmlKnowledge.h"
#include "Containers/StringFormat.h"
#include "UI/Tools/AssetEditors/RmlUiEditor/RmlBindingAnalysis.h"

namespace Lumina
{
    namespace RmlLanguageInternal
    {
        enum class EMode : uint32
        {
            Text,
            Comment,
            Tag,
            AttrDouble,
            AttrSingle,
            Css,
            Script,
        };

        constexpr uint32 ModeMask         = 0xFu;
        constexpr uint32 DepthShift       = 4;
        constexpr uint32 DepthMask        = 0xFu << DepthShift;
        constexpr uint32 ContainerShift   = 8;
        constexpr uint32 ContainerMask    = 0xFu << ContainerShift;
        constexpr uint32 InValue          = 1u << 12;
        constexpr uint32 CssComment       = 1u << 13;
        constexpr uint32 StyleTag         = 1u << 14;
        constexpr uint32 ScriptTag        = 1u << 15;
        constexpr uint32 InlineStyle      = 1u << 16;
        constexpr uint32 BindingValue     = 1u << 17;
        constexpr uint32 ClosingTag       = 1u << 18;
        constexpr uint32 ContainerPending = 1u << 19;
        constexpr uint32 TagBits          = StyleTag | ScriptTag | InlineStyle | BindingValue | ClosingTag;

        EMode ModeOf(uint32 State) { return (EMode)(State & ModeMask); }
        void SetMode(uint32& State, EMode Mode) { State = (State & ~ModeMask) | (uint32)Mode; }
        int32 DepthOf(uint32 State) { return (int32)((State & DepthMask) >> DepthShift); }
        void SetDepth(uint32& State, int32 Depth) { State = (State & ~DepthMask) | ((uint32)Math::Clamp(Depth, 0, 15) << DepthShift); }
        void SetFlag(uint32& State, uint32 Flag, bool bOn) { State = bOn ? (State | Flag) : (State & ~Flag); }

        bool IsContainerLevel(uint32 State, int32 Level)
        {
            return Level >= 1 && Level <= 4 && (State & (1u << (ContainerShift + Level - 1))) != 0;
        }

        void SetContainerLevel(uint32& State, int32 Level, bool bOn)
        {
            if (Level >= 1 && Level <= 4)
            {
                SetFlag(State, 1u << (ContainerShift + Level - 1), bOn);
            }
        }

        // Rules hold selectors at the top level and inside @media or @keyframes, declarations everywhere else.
        bool InSelectorContext(uint32 State)
        {
            const int32 Depth = DepthOf(State);
            return Depth == 0 || IsContainerLevel(State, Depth);
        }

        bool IsCssIdentChar(char C)
        {
            return CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '-' || C == '_' || (uint8)C >= 0x80;
        }

        bool IsNameChar(char C)
        {
            return CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '-' || C == '_' || C == ':' || C == '.' || (uint8)C >= 0x80;
        }

        bool IEquals(FStringView A, FStringView B)
        {
            return EqualsIgnoreCase(A, B);
        }

        int32 SkipIdent(FStringView Line, int32 i, int32 End)
        {
            while (i < End && IsCssIdentChar(Line[i]))
            {
                ++i;
            }
            return i;
        }

        void Mark(ECodeToken* Out, int32 From, int32 To, ECodeToken Token)
        {
            for (int32 i = From; i < To; ++i)
            {
                Out[i] = Token;
            }
        }

        void SplitWords(FStringView List, TVector<FStringView>& Out)
        {
            size_t Start = 0;
            while (Start < List.size())
            {
                size_t End = List.find(' ', Start);
                End = End == FStringView::npos ? List.size() : End;
                if (End > Start)
                {
                    Out.push_back(List.substr(Start, End - Start));
                }
                Start = End + 1;
            }
        }

        void AddUnique(TVector<FString>& List, FStringView Value)
        {
            if (!Value.empty() && !Algo::AnyOf(List.begin(), List.end(), [Value](const FString& Existing) { return Existing.View() == Value; }))
            {
                List.emplace_back(Value);
            }
        }

        // Collects the .class and #id names a stylesheet's selectors use, skipping colors inside declaration blocks.
        void ScanStylesheetSelectors(FStringView Css, TVector<FString>& OutClasses, TVector<FString>& OutIds)
        {
            TVector<bool> Blocks;
            bool bContainerPending = false;
            size_t i = 0;
            while (i < Css.size())
            {
                const char C = Css[i];
                if (C == '/' && i + 1 < Css.size() && Css[i + 1] == '*')
                {
                    const size_t Close = Css.find("*/", i + 2);
                    i = Close == FStringView::npos ? Css.size() : Close + 2;
                    continue;
                }
                if (C == '"' || C == '\'')
                {
                    const size_t Close = Css.find(C, i + 1);
                    i = Close == FStringView::npos ? Css.size() : Close + 1;
                    continue;
                }
                const bool bSelector = Blocks.empty() || Blocks.back();
                if (C == '@')
                {
                    const int32 End = SkipIdent(Css, (int32)i + 1, (int32)Css.size());
                    const FStringView Rule = Css.substr(i + 1, End - i - 1);
                    bContainerPending = IEquals(Rule, "media") || IEquals(Rule, "keyframes");
                    i = End;
                    continue;
                }
                if (C == '{')
                {
                    Blocks.push_back(bSelector && bContainerPending);
                    bContainerPending = false;
                }
                else if (C == '}')
                {
                    if (!Blocks.empty())
                    {
                        Blocks.pop_back();
                    }
                }
                else if (bSelector && (C == '.' || C == '#') && i + 1 < Css.size() && IsCssIdentChar(Css[i + 1]) && !CodeText::IsDigit(Css[i + 1]))
                {
                    const int32 End = SkipIdent(Css, (int32)i + 1, (int32)Css.size());
                    AddUnique(C == '.' ? OutClasses : OutIds, Css.substr(i + 1, End - i - 1));
                    i = End;
                    continue;
                }
                ++i;
            }
        }

        // Each value of attribute Name across the document, as in every class="..." it holds.
        void ScanAttributeValues(FStringView Text, FStringView Name, TVector<FString>& Out)
        {
            size_t From = 0;
            while (true)
            {
                const int32 Found = CodeText::FindIgnoreCase(Text, Name, (int32)From);
                if (Found < 0)
                {
                    return;
                }
                size_t i = Found + Name.size();
                From = i;
                const bool bBoundary = Found == 0 || CodeText::IsSpace(Text[Found - 1]) || Text[Found - 1] == '\n';
                while (i < Text.size() && CodeText::IsSpace(Text[i]))
                {
                    ++i;
                }
                if (!bBoundary || i >= Text.size() || Text[i] != '=')
                {
                    continue;
                }
                ++i;
                while (i < Text.size() && CodeText::IsSpace(Text[i]))
                {
                    ++i;
                }
                if (i >= Text.size() || (Text[i] != '"' && Text[i] != '\''))
                {
                    continue;
                }
                const size_t Close = Text.find(Text[i], i + 1);
                if (Close == FStringView::npos)
                {
                    return;
                }
                TVector<FStringView> Words;
                SplitWords(Text.substr(i + 1, Close - i - 1), Words);
                for (const FStringView Word : Words)
                {
                    if (Word.find('{') == FStringView::npos)
                    {
                        AddUnique(Out, Word);
                    }
                }
                From = Close + 1;
            }
        }

        int32 EditDistance(FStringView A, FStringView B)
        {
            constexpr int32 MaxLength = 48;
            if (A.size() > MaxLength || B.size() > MaxLength)
            {
                return MaxLength;
            }
            int32 Row[MaxLength + 1];
            for (int32 j = 0; j <= (int32)B.size(); ++j)
            {
                Row[j] = j;
            }
            for (int32 i = 1; i <= (int32)A.size(); ++i)
            {
                int32 Diagonal = Row[0];
                Row[0] = i;
                for (int32 j = 1; j <= (int32)B.size(); ++j)
                {
                    const int32 Above = Row[j];
                    Row[j] = Math::Min(Math::Min(Row[j] + 1, Row[j - 1] + 1), Diagonal + (A[i - 1] == B[j - 1] ? 0 : 1));
                    Diagonal = Above;
                }
            }
            return Row[B.size()];
        }

        uint32 HslToColor(float Hue, float Saturation, float Lightness, float Alpha)
        {
            const float Chroma = (1.0f - std::fabs(2.0f * Lightness - 1.0f)) * Saturation;
            const float Sector = std::fmod(std::fmod(Hue, 360.0f) + 360.0f, 360.0f) / 60.0f;
            const float X = Chroma * (1.0f - std::fabs(std::fmod(Sector, 2.0f) - 1.0f));
            const int32 Index = (int32)Sector;
            const float Table[6][3] = { { Chroma, X, 0 }, { X, Chroma, 0 }, { 0, Chroma, X }, { 0, X, Chroma }, { X, 0, Chroma }, { Chroma, 0, X } };
            const float M = Lightness - Chroma * 0.5f;
            return ImGui::ColorConvertFloat4ToU32(ImVec4(Table[Index % 6][0] + M, Table[Index % 6][1] + M, Table[Index % 6][2] + M, Alpha));
        }

        // Reads rgb(), rgba(), hsl() or hsla() arguments the way RmlUi does, where rgba alpha runs from 0 to 255.
        bool ParseColorFunction(FStringView Name, FStringView Arguments, uint32& OutColor)
        {
            float Values[4] = { 0.0f, 0.0f, 0.0f, -1.0f };
            bool bPercent[4] = {};
            int32 Count = 0;
            size_t i = 0;
            while (i < Arguments.size() && Count < 4)
            {
                while (i < Arguments.size() && (CodeText::IsSpace(Arguments[i]) || Arguments[i] == ',' || Arguments[i] == '/'))
                {
                    ++i;
                }
                const size_t Start = i;
                while (i < Arguments.size() && (CodeText::IsDigit(Arguments[i]) || Arguments[i] == '.' || Arguments[i] == '-'))
                {
                    ++i;
                }
                if (i == Start)
                {
                    return false;
                }
                const FString Number(Arguments.substr(Start, i - Start));
                Values[Count] = (float)std::atof(Number.c_str());
                while (i < Arguments.size() && (CodeText::IsAlpha(Arguments[i]) || Arguments[i] == '%'))
                {
                    bPercent[Count] |= Arguments[i] == '%';
                    ++i;
                }
                ++Count;
            }
            if (Count < 3)
            {
                return false;
            }
            const bool bHsl = CodeText::StartsWithIgnoreCase(Name, "hsl");
            if (bHsl)
            {
                const float Alpha = Count == 4 ? Math::Clamp(Values[3], 0.0f, 1.0f) : 1.0f;
                OutColor = HslToColor(Values[0], Values[1] * 0.01f, Values[2] * 0.01f, Alpha);
                return true;
            }
            uint32 Channels[4] = { 0, 0, 0, 255 };
            for (int32 c = 0; c < Count; ++c)
            {
                const float Value = bPercent[c] ? Values[c] * 2.55f : Values[c];
                Channels[c] = (uint32)Math::Clamp((int32)Value, 0, 255);
            }
            OutColor = IM_COL32(Channels[0], Channels[1], Channels[2], Channels[3]);
            return true;
        }

        FString DescribeColor(uint32 Color)
        {
            return Format("rgba({}, {}, {}, {})", (Color >> IM_COL32_R_SHIFT) & 0xFF, (Color >> IM_COL32_G_SHIFT) & 0xFF, (Color >> IM_COL32_B_SHIFT) & 0xFF, (Color >> IM_COL32_A_SHIFT) & 0xFF);
        }
    }

    using namespace RmlLanguageInternal;
    using namespace RmlKnowledge;

    FRmlLanguage::FRmlLanguage(bool bInStylesheet)
        : bStylesheet(bInStylesheet)
    {
        for (const FRmlProperty& Property : Properties())
        {
            PropertyMap.insert({ FStringView(Property.Name), &Property });
        }
        for (const FRmlFunction& Function : Functions())
        {
            FunctionMap.insert({ FStringView(Function.Name), &Function });
        }
        for (const FRmlNamedColor& Color : Colors())
        {
            ColorMap.insert({ FStringView(Color.Name), IM_COL32(Color.R, Color.G, Color.B, Color.A) });
        }
    }

    uint32 FRmlLanguage::GetInitialState() const
    {
        return (uint32)(bStylesheet ? EMode::Css : EMode::Text);
    }

    bool FRmlLanguage::IsWordChar(char C) const
    {
        return IsCssIdentChar(C);
    }

    const FRmlProperty* FRmlLanguage::FindProperty(FStringView Name) const
    {
        const auto Found = PropertyMap.find(Name);
        return Found != PropertyMap.end() ? Found->second : nullptr;
    }

    uint32 FRmlLanguage::Tokenize(FStringView Line, uint32 State, ECodeToken* Out) const
    {
        const int32 Size = (int32)Line.size();
        int32 i = 0;
        while (i < Size)
        {
            const int32 Before = i;
            const uint32 StateBefore = State;
            switch (ModeOf(State))
            {
                case EMode::Comment:
                {
                    const size_t Close = Line.find("-->", i);
                    const int32 End = Close == FStringView::npos ? Size : (int32)Close + 3;
                    Mark(Out, i, End, ECodeToken::Comment);
                    i = End;
                    if (Close != FStringView::npos)
                    {
                        SetMode(State, EMode::Text);
                    }
                    break;
                }
                case EMode::Script:
                {
                    const int32 Close = CodeText::FindIgnoreCase(Line, "</script", i);
                    const int32 End = Close < 0 ? Size : Close;
                    Mark(Out, i, End, ECodeToken::Default);
                    i = End;
                    if (Close >= 0)
                    {
                        SetMode(State, EMode::Text);
                    }
                    break;
                }
                case EMode::Text:
                    i = TokenizeText(Line, i, State, Out);
                    break;
                case EMode::Tag:
                    i = TokenizeTag(Line, i, State, Out);
                    break;
                case EMode::AttrDouble:
                case EMode::AttrSingle:
                    i = TokenizeAttributeValue(Line, i, State, Out);
                    break;
                case EMode::Css:
                    i = TokenizeStylesheet(Line, i, State, Out);
                    break;
            }
            if (i == Before && State == StateBefore)
            {
                Out[i++] = ECodeToken::Default;
            }
        }
        return State;
    }

    int32 FRmlLanguage::TokenizeText(FStringView Line, int32 i, uint32& State, ECodeToken* Out) const
    {
        const int32 Size = (int32)Line.size();
        while (i < Size)
        {
            const char C = Line[i];
            const char Next = i + 1 < Size ? Line[i + 1] : 0;
            if (C == '<')
            {
                if (Line.substr(i).starts_with("<!--"))
                {
                    Mark(Out, i, i + 4, ECodeToken::Comment);
                    SetMode(State, EMode::Comment);
                    return i + 4;
                }
                const bool bClosing = Next == '/';
                const int32 NameStart = i + (bClosing ? 2 : 1);
                const bool bNameNext = NameStart < Size && (CodeText::IsAlpha(Line[NameStart]) || Line[NameStart] == '!' || Line[NameStart] == '?');
                if (bClosing || bNameNext)
                {
                    Mark(Out, i, NameStart, ECodeToken::Punctuation);
                    int32 NameEnd = NameStart;
                    while (NameEnd < Size && (IsNameChar(Line[NameEnd]) || Line[NameEnd] == '!' || Line[NameEnd] == '?'))
                    {
                        ++NameEnd;
                    }
                    Mark(Out, NameStart, NameEnd, ECodeToken::Tag);
                    const FStringView Name = Line.substr(NameStart, NameEnd - NameStart);
                    State &= ~TagBits;
                    SetFlag(State, ClosingTag, bClosing);
                    SetFlag(State, StyleTag, !bClosing && IEquals(Name, "style"));
                    SetFlag(State, ScriptTag, !bClosing && IEquals(Name, "script"));
                    SetMode(State, EMode::Tag);
                    return NameEnd;
                }
            }
            if (C == '&')
            {
                int32 End = i + 1;
                while (End < Size && End - i < 12 && (CodeText::IsAlpha(Line[End]) || CodeText::IsDigit(Line[End]) || Line[End] == '#'))
                {
                    ++End;
                }
                if (End < Size && Line[End] == ';' && End > i + 1)
                {
                    Mark(Out, i, End + 1, ECodeToken::Keyword);
                    i = End + 1;
                    continue;
                }
            }
            if (C == '{' && Next == '{')
            {
                const size_t Close = Line.find("}}", i + 2);
                const int32 InnerEnd = Close == FStringView::npos ? Size : (int32)Close;
                Mark(Out, i, i + 2, ECodeToken::Punctuation);
                Mark(Out, i + 2, InnerEnd, ECodeToken::Binding);
                if (Close != FStringView::npos)
                {
                    Mark(Out, InnerEnd, InnerEnd + 2, ECodeToken::Punctuation);
                }
                i = Close == FStringView::npos ? Size : InnerEnd + 2;
                continue;
            }
            Out[i++] = ECodeToken::Default;
        }
        return i;
    }

    int32 FRmlLanguage::TokenizeTag(FStringView Line, int32 i, uint32& State, ECodeToken* Out) const
    {
        const int32 Size = (int32)Line.size();
        bool bAfterEquals = false;
        while (i < Size)
        {
            const char C = Line[i];
            const char Next = i + 1 < Size ? Line[i + 1] : 0;
            if (CodeText::IsSpace(C))
            {
                Out[i++] = ECodeToken::Default;
                continue;
            }
            if ((C == '/' || C == '?') && Next == '>')
            {
                Mark(Out, i, i + 2, ECodeToken::Punctuation);
                State &= ~TagBits;
                SetMode(State, EMode::Text);
                return i + 2;
            }
            if (C == '>')
            {
                Out[i] = ECodeToken::Punctuation;
                const bool bClosing = (State & ClosingTag) != 0;
                const bool bStyle = !bClosing && (State & StyleTag) != 0;
                const bool bScript = !bClosing && (State & ScriptTag) != 0;
                State &= ~(TagBits | InValue);
                if (bStyle)
                {
                    SetMode(State, EMode::Css);
                    SetDepth(State, 0);
                    State &= ~(ContainerMask | ContainerPending | CssComment);
                }
                else
                {
                    SetMode(State, bScript ? EMode::Script : EMode::Text);
                }
                return i + 1;
            }
            if (C == '=')
            {
                Out[i++] = ECodeToken::Punctuation;
                bAfterEquals = true;
                continue;
            }
            if (C == '"' || C == '\'')
            {
                Out[i] = ECodeToken::String;
                SetMode(State, C == '"' ? EMode::AttrDouble : EMode::AttrSingle);
                return i + 1;
            }
            const int32 Start = i;
            while (i < Size && !CodeText::IsSpace(Line[i]) && Line[i] != '=' && Line[i] != '>' && Line[i] != '"' && Line[i] != '\'' && !(Line[i] == '/' && i + 1 < Size && Line[i + 1] == '>'))
            {
                ++i;
            }
            if (i == Start)
            {
                Out[i++] = ECodeToken::Punctuation;
                continue;
            }
            if (bAfterEquals)
            {
                Mark(Out, Start, i, ECodeToken::String);
                bAfterEquals = false;
                continue;
            }
            const FStringView Name = Line.substr(Start, i - Start);
            Mark(Out, Start, i, ECodeToken::Attribute);
            SetFlag(State, InlineStyle, IEquals(Name, "style"));
            SetFlag(State, BindingValue, CodeText::StartsWithIgnoreCase(Name, "data-") || (CodeText::StartsWithIgnoreCase(Name, "on") && Name.size() > 2));
        }
        return i;
    }

    int32 FRmlLanguage::TokenizeAttributeValue(FStringView Line, int32 i, uint32& State, ECodeToken* Out) const
    {
        const char Quote = ModeOf(State) == EMode::AttrDouble ? '"' : '\'';
        const size_t Close = Line.find(Quote, i);
        const int32 End = Close == FStringView::npos ? (int32)Line.size() : (int32)Close;
        if (State & InlineStyle)
        {
            int32 j = i;
            while (j < End)
            {
                j = TokenizeDeclaration(Line, j, End, State, Out);
            }
        }
        else
        {
            Mark(Out, i, End, (State & BindingValue) ? ECodeToken::Binding : ECodeToken::String);
        }
        if (Close == FStringView::npos)
        {
            return End;
        }
        Out[End] = ECodeToken::String;
        State &= ~(InlineStyle | BindingValue | InValue);
        SetMode(State, EMode::Tag);
        return End + 1;
    }

    int32 FRmlLanguage::TokenizeDeclaration(FStringView Line, int32 i, int32 End, uint32& State, ECodeToken* Out) const
    {
        const char C = Line[i];
        const char Next = i + 1 < End ? Line[i + 1] : 0;
        if (CodeText::IsSpace(C))
        {
            Out[i] = ECodeToken::Default;
            return i + 1;
        }
        if (C == '/' && Next == '*')
        {
            const size_t Close = Line.find("*/", i + 2);
            const int32 CommentEnd = (Close == FStringView::npos || (int32)Close + 2 > End) ? End : (int32)Close + 2;
            Mark(Out, i, CommentEnd, ECodeToken::Comment);
            return CommentEnd;
        }
        if (C == ';')
        {
            Out[i] = ECodeToken::Punctuation;
            State &= ~InValue;
            return i + 1;
        }
        if (!(State & InValue))
        {
            if (C == ':')
            {
                Out[i] = ECodeToken::Punctuation;
                State |= InValue;
                return i + 1;
            }
            if (IsCssIdentChar(C))
            {
                const int32 IdentEnd = SkipIdent(Line, i, End);
                Mark(Out, i, IdentEnd, ECodeToken::Property);
                return IdentEnd;
            }
            Out[i] = ECodeToken::Punctuation;
            return i + 1;
        }
        if (C == '"' || C == '\'')
        {
            int32 StringEnd = i + 1;
            while (StringEnd < End && Line[StringEnd] != C)
            {
                ++StringEnd;
            }
            StringEnd = Math::Min(StringEnd + 1, End);
            Mark(Out, i, StringEnd, ECodeToken::String);
            return StringEnd;
        }
        if (C == '#')
        {
            const int32 HashEnd = SkipIdent(Line, i + 1, End);
            Mark(Out, i, HashEnd, ECodeToken::Number);
            return HashEnd;
        }
        const bool bSigned = (C == '-' || C == '+') && (CodeText::IsDigit(Next) || Next == '.');
        if (CodeText::IsDigit(C) || (C == '.' && CodeText::IsDigit(Next)) || bSigned)
        {
            int32 NumberEnd = i + 1;
            while (NumberEnd < End && (CodeText::IsDigit(Line[NumberEnd]) || Line[NumberEnd] == '.'))
            {
                ++NumberEnd;
            }
            while (NumberEnd < End && (CodeText::IsAlpha(Line[NumberEnd]) || Line[NumberEnd] == '%'))
            {
                ++NumberEnd;
            }
            Mark(Out, i, NumberEnd, ECodeToken::Number);
            return NumberEnd;
        }
        if (C == '!' && CodeText::IsAlpha(Next))
        {
            const int32 KeywordEnd = SkipIdent(Line, i + 1, End);
            Mark(Out, i, KeywordEnd, ECodeToken::Keyword);
            return KeywordEnd;
        }
        if (IsCssIdentChar(C))
        {
            const int32 IdentEnd = SkipIdent(Line, i, End);
            Mark(Out, i, IdentEnd, IdentEnd < End && Line[IdentEnd] == '(' ? ECodeToken::Function : ECodeToken::Value);
            return IdentEnd;
        }
        Out[i] = ECodeToken::Punctuation;
        return i + 1;
    }

    int32 FRmlLanguage::TokenizeStylesheet(FStringView Line, int32 i, uint32& State, ECodeToken* Out) const
    {
        const int32 Size = (int32)Line.size();
        bool bPrelude = false;
        while (i < Size)
        {
            if (State & CssComment)
            {
                const size_t Close = Line.find("*/", i);
                const int32 End = Close == FStringView::npos ? Size : (int32)Close + 2;
                Mark(Out, i, End, ECodeToken::Comment);
                i = End;
                if (Close != FStringView::npos)
                {
                    State &= ~CssComment;
                }
                continue;
            }
            const char C = Line[i];
            const char Next = i + 1 < Size ? Line[i + 1] : 0;
            if (CodeText::IsSpace(C))
            {
                Out[i++] = ECodeToken::Default;
                continue;
            }
            if (C == '/' && Next == '*')
            {
                Mark(Out, i, i + 2, ECodeToken::Comment);
                State |= CssComment;
                i += 2;
                continue;
            }
            if (!bStylesheet && C == '<' && Next == '/' && CodeText::StartsWithIgnoreCase(Line.substr(i + 2), "style"))
            {
                SetMode(State, EMode::Text);
                State &= ~(InValue | ContainerPending | ContainerMask | DepthMask);
                return i;
            }

            const bool bSelector = InSelectorContext(State);
            if (C == '{')
            {
                Out[i++] = ECodeToken::Punctuation;
                const int32 Depth = DepthOf(State) + 1;
                SetDepth(State, Depth);
                SetContainerLevel(State, Depth, bSelector && (State & ContainerPending) != 0);
                State &= ~(ContainerPending | InValue);
                bPrelude = false;
                continue;
            }
            if (C == '}')
            {
                Out[i++] = ECodeToken::Punctuation;
                const int32 Depth = DepthOf(State);
                SetContainerLevel(State, Depth, false);
                SetDepth(State, Depth - 1);
                State &= ~InValue;
                continue;
            }
            if (C == ';')
            {
                Out[i++] = ECodeToken::Punctuation;
                State &= ~(InValue | ContainerPending);
                bPrelude = false;
                continue;
            }
            if (!bSelector)
            {
                i = TokenizeDeclaration(Line, i, Size, State, Out);
                continue;
            }

            if (C == '@')
            {
                const int32 End = SkipIdent(Line, i + 1, Size);
                Mark(Out, i, End, ECodeToken::Keyword);
                const FStringView Rule = Line.substr(i + 1, End - i - 1);
                SetFlag(State, ContainerPending, IEquals(Rule, "media") || IEquals(Rule, "keyframes"));
                bPrelude = true;
                i = End;
                continue;
            }
            if (!bPrelude && (C == '.' || C == '#') && IsCssIdentChar(Next))
            {
                const int32 End = SkipIdent(Line, i + 1, Size);
                Mark(Out, i, End, ECodeToken::Selector);
                i = End;
                continue;
            }
            if (!bPrelude && C == ':')
            {
                int32 End = i + 1 < Size && Line[i + 1] == ':' ? i + 2 : i + 1;
                End = SkipIdent(Line, End, Size);
                Mark(Out, i, End, ECodeToken::Keyword);
                if (End < Size && Line[End] == '(')
                {
                    const size_t Close = Line.find(')', End);
                    const int32 ArgumentsEnd = Close == FStringView::npos ? Size : (int32)Close + 1;
                    Mark(Out, End, ArgumentsEnd, ECodeToken::Number);
                    Out[End] = ECodeToken::Punctuation;
                    if (Close != FStringView::npos)
                    {
                        Out[Close] = ECodeToken::Punctuation;
                    }
                    End = ArgumentsEnd;
                }
                i = End;
                continue;
            }
            if (C == '[')
            {
                const size_t Close = Line.find(']', i);
                const int32 End = Close == FStringView::npos ? Size : (int32)Close + 1;
                Mark(Out, i, End, ECodeToken::Attribute);
                Out[i] = ECodeToken::Punctuation;
                if (Close != FStringView::npos)
                {
                    Out[Close] = ECodeToken::Punctuation;
                }
                i = End;
                continue;
            }
            if (C == '"' || C == '\'')
            {
                int32 End = i + 1;
                while (End < Size && Line[End] != C)
                {
                    ++End;
                }
                End = Math::Min(End + 1, Size);
                Mark(Out, i, End, ECodeToken::String);
                i = End;
                continue;
            }
            if (CodeText::IsDigit(C))
            {
                int32 End = i;
                while (End < Size && (CodeText::IsDigit(Line[End]) || Line[End] == '.' || Line[End] == '%' || CodeText::IsAlpha(Line[End])))
                {
                    ++End;
                }
                Mark(Out, i, End, ECodeToken::Number);
                i = End;
                continue;
            }
            if (IsCssIdentChar(C))
            {
                const int32 End = SkipIdent(Line, i, Size);
                const FStringView Word = Line.substr(i, End - i);
                ECodeToken Token = ECodeToken::Tag;
                if (bPrelude)
                {
                    Token = ECodeToken::Value;
                }
                else if (IEquals(Word, "from") || IEquals(Word, "to"))
                {
                    Token = ECodeToken::Keyword;
                }
                Mark(Out, i, End, Token);
                i = End;
                continue;
            }
            Out[i++] = ECodeToken::Punctuation;
        }
        return i;
    }

    FCodeCommentSyntax FRmlLanguage::GetCommentSyntax(uint32 State) const
    {
        if (ModeOf(State) == EMode::Css || (State & InlineStyle))
        {
            return { {}, "/*", "*/" };
        }
        return { {}, "<!--", "-->" };
    }

    bool FRmlLanguage::ShouldIndentAfter(FStringView LineBeforeCaret, uint32 CaretState) const
    {
        if (ModeOf(CaretState) == EMode::Tag)
        {
            return true;
        }
        const size_t Last = LineBeforeCaret.find_last_not_of(" \t");
        if (Last == FStringView::npos)
        {
            return false;
        }
        const FStringView Trimmed = LineBeforeCaret.substr(0, Last + 1);
        const char C = Trimmed.back();
        if (C == '{' || C == '(' || C == '[')
        {
            return true;
        }
        if (C != '>' || Trimmed.ends_with("/>") || Trimmed.ends_with("-->"))
        {
            return false;
        }
        const size_t Open = Trimmed.rfind('<');
        if (Open == FStringView::npos || Open + 1 >= Trimmed.size() || !CodeText::IsAlpha(Trimmed[Open + 1]))
        {
            return false;
        }
        int32 NameEnd = (int32)Open + 1;
        while (NameEnd < (int32)Trimmed.size() && IsNameChar(Trimmed[NameEnd]))
        {
            ++NameEnd;
        }
        const FRmlElement* Element = FindElement(Trimmed.substr(Open + 1, NameEnd - Open - 1));
        return Element == nullptr || !Element->bVoid;
    }

    bool FRmlLanguage::ShouldOutdentOnType(FStringView LineThroughCaret, char Typed) const
    {
        const FStringView Content = LineThroughCaret.substr(CodeText::LeadingWhitespace(LineThroughCaret));
        return (Typed == '}' && Content == "}") || (Typed == '/' && Content == "</");
    }

    FString FRmlLanguage::GetAutoInsert(const FCodeDocument& Document, FCodePosition Caret, uint32 CaretState, char Typed) const
    {
        if (bStylesheet)
        {
            return {};
        }
        const FStringView Line = Document.GetLineView(Caret.Line);
        const FStringView Prefix = Line.substr(0, Caret.Column);
        const FStringView Rest = Line.substr(Caret.Column);
        const EMode Mode = ModeOf(CaretState);

        if (Typed == '>' && (Mode == EMode::Text || Mode == EMode::Css || Mode == EMode::Script) && !Prefix.ends_with("/>"))
        {
            FStringView TagLine = Prefix;
            size_t Open = TagLine.rfind('<');
            for (int32 Above = Caret.Line - 1; Open == FStringView::npos && Above >= Math::Max(0, Caret.Line - 20); --Above)
            {
                TagLine = Document.GetLineView(Above);
                Open = TagLine.rfind('<');
            }
            if (Open == FStringView::npos || Open + 1 >= TagLine.size() || !CodeText::IsAlpha(TagLine[Open + 1]))
            {
                return {};
            }
            size_t NameEnd = Open + 1;
            while (NameEnd < TagLine.size() && IsNameChar(TagLine[NameEnd]))
            {
                ++NameEnd;
            }
            const FStringView Name = TagLine.substr(Open + 1, NameEnd - Open - 1);
            const FRmlElement* Element = FindElement(Name);
            if (Element != nullptr && Element->bVoid)
            {
                return {};
            }
            const FString Closing = "</" + FString(Name) + ">";
            if (Rest.starts_with(Closing.View()))
            {
                return {};
            }
            return "$0" + Closing;
        }

        if (Typed == '/' && Prefix.ends_with("</") && (Rest.empty() || !IsNameChar(Rest[0])))
        {
            TVector<FOpenElement> Stack;
            BuildElementStack(Document, { Caret.Line, Caret.Column - 2 }, Stack, nullptr);
            if (Stack.empty())
            {
                return {};
            }
            return Stack.back().Name + ">$0";
        }

        if (Typed == '=' && Mode == EMode::Tag && Prefix.size() >= 2 && IsNameChar(Prefix[Prefix.size() - 2]) && (Rest.empty() || (Rest[0] != '"' && Rest[0] != '\'')))
        {
            return "\"$1\"$0";
        }
        return {};
    }

    bool FRmlLanguage::IsCompletionTrigger(char Typed, FStringView LinePrefix, uint32 CaretState) const
    {
        const char Before = LinePrefix.size() >= 2 ? LinePrefix[LinePrefix.size() - 2] : 0;
        switch (ModeOf(CaretState))
        {
            case EMode::Text:
                return Typed == '<' || Typed == '&';
            case EMode::Tag:
                if (Typed == '/')
                {
                    return Before == '<';
                }
                return Typed == ' ' && !CodeText::IsSpace(Before) && Before != '<';
            case EMode::AttrDouble:
            case EMode::AttrSingle:
                if (CaretState & InlineStyle)
                {
                    return Typed == ':' || (Typed == ' ' && (CaretState & InValue) != 0);
                }
                if (CaretState & BindingValue)
                {
                    return false;
                }
                return Typed == '"' || Typed == '\'' || Typed == '=' || Typed == ' ' || Typed == '#';
            case EMode::Css:
                if (CaretState & CssComment)
                {
                    return false;
                }
                if (InSelectorContext(CaretState))
                {
                    return Typed == '.' || Typed == '#' || Typed == ':' || Typed == '@';
                }
                return Typed == ':' || Typed == '!' || (Typed == ' ' && (CaretState & InValue) != 0);
            default:
                return false;
        }
    }

    void FRmlLanguage::GetCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const
    {
        const uint32 State = Request.CaretState;
        const FStringView Prefix = Request.LinePrefix;
        const int32 WordStart = Request.WordStart.Column;
        const char Before = WordStart > 0 ? Prefix[WordStart - 1] : 0;
        const char Before2 = WordStart > 1 ? Prefix[WordStart - 2] : 0;

        switch (ModeOf(State))
        {
            case EMode::Text:
            case EMode::Tag:
            {
                if (Before == '/' && Before2 == '<')
                {
                    TVector<FOpenElement> Stack;
                    BuildElementStack(*Request.Document, { Request.Caret.Line, WordStart - 2 }, Stack, nullptr);
                    const bool bCloseFollows = Request.Document->CharAt(Request.Caret) == '>';
                    for (int32 i = (int32)Stack.size() - 1; i >= 0; --i)
                    {
                        if (Algo::AnyOf(Out.begin(), Out.end(), [&Stack, i](const FCodeCompletionItem& Item) { return Item.Label == Stack[i].Name; }))
                        {
                            continue;
                        }
                        FCodeCompletionItem& Item = Out.emplace_back();
                        Item.Label = Stack[i].Name;
                        Item.InsertText = bCloseFollows ? Stack[i].Name : Stack[i].Name + ">";
                        Item.Kind = ECodeCompletionKind::Element;
                        Item.Detail = Format("closes line {}", Stack[i].Position.Line + 1);
                        Item.SortBoost = 100 - ((int32)Stack.size() - 1 - i);
                    }
                    return;
                }
                if (Before == '<')
                {
                    for (const FRmlElement& Element : Elements())
                    {
                        if (Element.bStyleOnly)
                        {
                            continue;
                        }
                        FCodeCompletionItem& Item = Out.emplace_back();
                        Item.Label = Element.Name;
                        Item.Kind = ECodeCompletionKind::Element;
                        Item.Documentation = Element.Documentation;
                        Item.Detail = Element.bVoid ? "void" : "";
                        const FStringView Name(Element.Name);
                        Item.SortBoost = (Name == "div" || Name == "span" || Name == "p" || Name == "button" || Name == "img" || Name == "input") ? 6 : 0;
                    }
                    FCodeCompletionItem& Comment = Out.emplace_back();
                    Comment.Label = "!--";
                    Comment.InsertText = "!-- $0 -->";
                    Comment.Kind = ECodeCompletionKind::Snippet;
                    Comment.Detail = "comment";
                    return;
                }
                if (ModeOf(State) == EMode::Tag)
                {
                    if ((State & ClosingTag) == 0)
                    {
                        AddAttributeCompletions(Request, Out);
                    }
                    return;
                }
                if (Before == '&')
                {
                    for (const FRmlNamed& Entity : Entities())
                    {
                        FCodeCompletionItem& Item = Out.emplace_back();
                        Item.Label = Entity.Name;
                        Item.Kind = ECodeCompletionKind::Keyword;
                        Item.Documentation = Entity.Documentation;
                    }
                    return;
                }
                if (Request.bExplicit)
                {
                    for (const FRmlNamed& Snippet : DocumentSnippets())
                    {
                        FCodeCompletionItem& Item = Out.emplace_back();
                        Item.Label = Snippet.Name;
                        Item.InsertText = Snippet.Insert;
                        Item.Documentation = Snippet.Documentation;
                        Item.Kind = ECodeCompletionKind::Snippet;
                    }
                }
                return;
            }
            case EMode::AttrDouble:
            case EMode::AttrSingle:
                if (State & InlineStyle)
                {
                    AddCssCompletions(Request, false, Out);
                }
                else if ((State & BindingValue) == 0)
                {
                    AddAttributeValueCompletions(Request, Out);
                }
                return;
            case EMode::Css:
                if ((State & CssComment) == 0)
                {
                    AddCssCompletions(Request, InSelectorContext(State), Out);
                }
                return;
            default:
                return;
        }
    }

    void FRmlLanguage::AddAttributeCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const
    {
        const FString ElementName = FindTagName(*Request.Document, Request.WordStart);
        const FRmlElement* Element = FindElement(ElementName.View());
        const bool bValueFollows = Request.Document->CharAt(Request.Caret) == '=';
        THashSet<FStringView> Added;

        auto Add = [&](FStringView Name, FStringView Documentation, bool bBoolean, ECodeCompletionKind Kind, int32 Boost, FStringView Detail)
        {
            if (!Added.insert(Name).second)
            {
                return;
            }
            FCodeCompletionItem& Item = Out.emplace_back();
            Item.Label = Name;
            Item.Kind = Kind;
            Item.Documentation = Documentation;
            Item.Detail = Detail;
            Item.SortBoost = Boost;
            if (!bBoolean && !bValueFollows && !Name.ends_with("-"))
            {
                Item.InsertText = FString(Name) + "=\"$1\"$0";
                Item.bRetrigger = true;
            }
        };

        if (Element != nullptr)
        {
            TVector<FStringView> Names;
            SplitWords(Element->Attributes, Names);
            for (const FStringView Name : Names)
            {
                const FRmlAttribute* Attribute = FindAttribute(ElementName.View(), Name);
                Add(Name, Attribute != nullptr ? FStringView(Attribute->Documentation) : FStringView(), Attribute != nullptr && Attribute->bBoolean, ECodeCompletionKind::Attribute, 20, Element->Name);
            }
        }
        for (const FRmlAttribute& Attribute : Attributes())
        {
            if (*Attribute.Element == '\0')
            {
                const FStringView Name(Attribute.Name);
                const bool bCore = Name == "id" || Name == "class" || Name == "style";
                Add(Name, Attribute.Documentation, Attribute.bBoolean, ECodeCompletionKind::Attribute, bCore ? 12 : 0, {});
            }
        }
        for (const auto& [Name, Description] : RmlBinding::KnownAttributes())
        {
            Add(Name, Description, false, ECodeCompletionKind::Attribute, 2, "data binding");
        }
        for (const FRmlNamed& Event : Events())
        {
            Add(Event.Name, Event.Documentation, false, ECodeCompletionKind::Event, -2, "event");
        }
    }

    void FRmlLanguage::AddAttributeValueCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const
    {
        const FStringView Line = Request.Document->GetLineView(Request.Caret.Line);
        const char Quote = ModeOf(Request.CaretState) == EMode::AttrDouble ? '"' : '\'';
        int32 QuoteAt = Request.WordStart.Column - 1;
        while (QuoteAt >= 0 && Line[QuoteAt] != Quote)
        {
            --QuoteAt;
        }
        int32 Back = QuoteAt - 1;
        while (Back >= 0 && CodeText::IsSpace(Line[Back]))
        {
            --Back;
        }
        if (QuoteAt < 0 || Back < 0 || Line[Back] != '=')
        {
            return;
        }
        --Back;
        while (Back >= 0 && CodeText::IsSpace(Line[Back]))
        {
            --Back;
        }
        const int32 NameEnd = Back + 1;
        while (Back >= 0 && IsNameChar(Line[Back]))
        {
            --Back;
        }
        const FStringView AttributeName = Line.substr(Back + 1, NameEnd - Back - 1);
        const FString ElementName = FindTagName(*Request.Document, { Request.Caret.Line, Back + 1 });
        const FStringView ValueSoFar = Line.substr(QuoteAt + 1, Request.WordStart.Column - QuoteAt - 1);
        const char Before = Request.WordStart.Column > 0 ? Line[Request.WordStart.Column - 1] : 0;

        auto Add = [&Out](FStringView Label, FStringView Insert, ECodeCompletionKind Kind, FStringView Detail, FStringView Documentation)
        {
            FCodeCompletionItem& Item = Out.emplace_back();
            Item.Label = Label;
            Item.InsertText = Insert;
            Item.Kind = Kind;
            Item.Detail = Detail;
            Item.Documentation = Documentation;
        };

        if (IEquals(AttributeName, "class"))
        {
            TVector<FString> Classes;
            TVector<FString> Ids;
            CollectSelectors(*Request.Document, Classes, Ids);
            TVector<FStringView> Present;
            SplitWords(ValueSoFar, Present);
            for (const FString& Class : Classes)
            {
                if (!Algo::AnyOf(Present.begin(), Present.end(), [&Class](FStringView Word) { return Word == Class.View(); }))
                {
                    Add(Class.View(), {}, ECodeCompletionKind::Selector, "class", {});
                }
            }
            return;
        }

        const bool bTarget = IEquals(AttributeName, "move_target") || IEquals(AttributeName, "size_target");
        if (bTarget || IEquals(AttributeName, "for") || IEquals(AttributeName, "nav-up"))
        {
            TVector<FString> Classes;
            TVector<FString> Ids;
            CollectSelectors(*Request.Document, Classes, Ids);
            const FStringView Hash = Before == '#' ? FStringView() : FStringView("#");
            if (bTarget)
            {
                for (const char* Keyword : { "self", "parent", "document" })
                {
                    Add(Keyword, FString(Hash) + Keyword, ECodeCompletionKind::Value, "target", {});
                }
            }
            for (const FString& Id : Ids)
            {
                Add(Id.View(), bTarget ? (FString(Hash) + Id).View() : Id.View(), ECodeCompletionKind::Selector, "id", {});
            }
            return;
        }

        const FRmlAttribute* Attribute = FindAttribute(ElementName.View(), AttributeName);
        if (Attribute == nullptr || *Attribute->Values == '\0')
        {
            return;
        }
        TVector<FStringView> Values;
        SplitWords(Attribute->Values, Values);
        for (const FStringView Value : Values)
        {
            Add(Value, {}, ECodeCompletionKind::Value, AttributeName, Attribute->Documentation);
        }
    }

    void FRmlLanguage::AddCssCompletions(const FCodeCompletionRequest& Request, bool bSelector, TVector<FCodeCompletionItem>& Out) const
    {
        const FStringView Prefix = Request.LinePrefix;
        const int32 WordStart = Request.WordStart.Column;
        const char Before = WordStart > 0 ? Prefix[WordStart - 1] : 0;

        if (bSelector)
        {
            if (Before == '@')
            {
                for (const FRmlNamed& Rule : AtRules())
                {
                    FCodeCompletionItem& Item = Out.emplace_back();
                    Item.Label = Rule.Name;
                    Item.InsertText = Rule.Insert;
                    Item.Documentation = Rule.Documentation;
                    Item.Kind = ECodeCompletionKind::Keyword;
                    Item.Detail = "at-rule";
                }
                return;
            }
            if (Before == '.' || Before == '#')
            {
                TVector<FString> Classes;
                TVector<FString> Ids;
                CollectSelectors(*Request.Document, Classes, Ids);
                for (const FString& Name : Before == '.' ? Classes : Ids)
                {
                    if (Name.View() != Request.Word)
                    {
                        FCodeCompletionItem& Item = Out.emplace_back();
                        Item.Label = Name;
                        Item.Kind = ECodeCompletionKind::Selector;
                        Item.Detail = Before == '.' ? "class" : "id";
                    }
                }
                return;
            }
            if (Before == ':')
            {
                for (const FRmlNamed& Pseudo : PseudoClasses())
                {
                    FCodeCompletionItem& Item = Out.emplace_back();
                    Item.Label = Pseudo.Name;
                    Item.InsertText = Pseudo.Insert;
                    Item.Documentation = Pseudo.Documentation;
                    Item.Kind = ECodeCompletionKind::Keyword;
                    Item.Detail = "pseudo-class";
                }
                return;
            }
            if (Request.Word.empty() && !Request.bExplicit)
            {
                return;
            }
            for (const FRmlElement& Element : Elements())
            {
                FCodeCompletionItem& Item = Out.emplace_back();
                Item.Label = Element.Name;
                Item.Kind = ECodeCompletionKind::Element;
                Item.Documentation = Element.Documentation;
                Item.Detail = "element";
                Item.SortBoost = Element.bStyleOnly ? -3 : 0;
            }
            return;
        }

        if ((Request.CaretState & InValue) == 0)
        {
            const FStringView Line = Request.Document->GetLineView(Request.Caret.Line);
            FStringView After = Line.substr(Request.Caret.Column);
            After = After.substr(Math::Min(After.size(), (size_t)SkipIdent(After, 0, (int32)After.size())));
            After = After.substr(CodeText::LeadingWhitespace(After));
            const bool bColonFollows = After.starts_with(":");
            const bool bSemicolonFollows = After.starts_with(";");
            for (const FRmlProperty& Property : Properties())
            {
                FCodeCompletionItem& Item = Out.emplace_back();
                Item.Label = Property.Name;
                Item.Kind = ECodeCompletionKind::Property;
                Item.Detail = Property.Syntax;
                Item.Documentation = Property.Documentation;
                if (!bColonFollows)
                {
                    Item.InsertText = FString(Property.Name) + (bSemicolonFollows ? ": $0" : ": $0;");
                    Item.bRetrigger = true;
                }
            }
            if (!Request.Word.empty() || Request.bExplicit)
            {
                for (const FRmlNamed& Snippet : StyleSnippets())
                {
                    FCodeCompletionItem& Item = Out.emplace_back();
                    Item.Label = Snippet.Name;
                    Item.InsertText = Snippet.Insert;
                    Item.Documentation = Snippet.Documentation;
                    Item.Kind = ECodeCompletionKind::Snippet;
                    Item.SortBoost = -4;
                }
            }
            return;
        }

        int32 Depth = 0;
        int32 Colon = -1;
        for (int32 i = WordStart - 1; i >= 0; --i)
        {
            const char C = Prefix[i];
            if (C == ')')
            {
                ++Depth;
            }
            else if (C == '(')
            {
                Depth = Math::Max(0, Depth - 1);
            }
            else if (Depth == 0 && C == ':')
            {
                Colon = i;
                break;
            }
            else if (C == ';' || C == '{' || C == '"' || C == '\'')
            {
                break;
            }
        }
        FStringView Property;
        if (Colon >= 0)
        {
            int32 End = Colon;
            while (End > 0 && CodeText::IsSpace(Prefix[End - 1]))
            {
                --End;
            }
            int32 Start = End;
            while (Start > 0 && IsCssIdentChar(Prefix[Start - 1]))
            {
                --Start;
            }
            Property = Prefix.substr(Start, End - Start);
        }
        AddValueCompletions(Request, Property, Out);
    }

    void FRmlLanguage::AddValueCompletions(const FCodeCompletionRequest& Request, FStringView PropertyName, TVector<FCodeCompletionItem>& Out) const
    {
        const FRmlProperty* Property = FindProperty(PropertyName);
        const FStringView Word = Request.Word;
        const int32 WordStart = Request.WordStart.Column;
        const char Before = WordStart > 0 ? Request.LinePrefix[WordStart - 1] : 0;

        if (Before == '!')
        {
            FCodeCompletionItem& Item = Out.emplace_back();
            Item.Label = "important";
            Item.Kind = ECodeCompletionKind::Keyword;
            Item.Documentation = "Wins over any declaration of this property without !important.";
            return;
        }

        const uint32 Values = Property != nullptr ? Property->Values : (EValue::Color | EValue::Length);
        const bool bNumeric = !Word.empty() && (CodeText::IsDigit(Word[0]) || (Word[0] == '-' && Word.size() > 1 && CodeText::IsDigit(Word[1])));
        if (bNumeric)
        {
            size_t NumberEnd = Word[0] == '-' ? 1 : 0;
            while (NumberEnd < Word.size() && (CodeText::IsDigit(Word[NumberEnd]) || Word[NumberEnd] == '.'))
            {
                ++NumberEnd;
            }
            const FStringView Number = Word.substr(0, NumberEnd);
            const bool bAny = Property == nullptr || (Values & (EValue::Transform | EValue::Filter | EValue::Decorator | EValue::FontEffect | EValue::Shadow)) != 0;
            auto AddUnit = [&](FStringView Unit, int32 Boost)
            {
                const FRmlNamed* Doc = FindNamed(Units(), Unit);
                FCodeCompletionItem& Item = Out.emplace_back();
                Item.Label = FString(Number) + Unit;
                Item.Kind = ECodeCompletionKind::Unit;
                Item.Detail = Unit;
                Item.Documentation = Doc != nullptr ? Doc->Documentation : "";
                Item.SortBoost = Boost;
            };
            if (bAny || (Values & EValue::Length))
            {
                AddUnit("dp", 10);
                AddUnit("px", 6);
                AddUnit("em", 2);
                AddUnit("rem", 0);
                AddUnit("vw", 0);
                AddUnit("vh", 0);
            }
            if (bAny || (Values & EValue::Percent))
            {
                AddUnit("%", 4);
            }
            if (Values & (EValue::Time | EValue::Tween))
            {
                AddUnit("s", 8);
                AddUnit("ms", 6);
            }
            if (bAny || (Values & EValue::Angle))
            {
                AddUnit("deg", 3);
                AddUnit("rad", 0);
            }
            return;
        }

        auto Add = [&Out](FStringView Label, ECodeCompletionKind Kind, FStringView Detail, FStringView Documentation, FStringView Insert, int32 Boost)
        {
            FCodeCompletionItem& Item = Out.emplace_back();
            Item.Label = Label;
            Item.Kind = Kind;
            Item.Detail = Detail;
            Item.Documentation = Documentation;
            Item.InsertText = Insert;
            Item.SortBoost = Boost;
            return &Item;
        };

        if (Property != nullptr)
        {
            TVector<FStringView> Keywords;
            SplitWords(Property->Keywords, Keywords);
            for (const FStringView Keyword : Keywords)
            {
                Add(Keyword, ECodeCompletionKind::Value, Property->Name, Property->Documentation, {}, 10);
            }
            Add("inherit", ECodeCompletionKind::Value, "keyword", "Takes the parent's value.", {}, -6);
        }
        if (Values & EValue::Color)
        {
            for (const FRmlNamedColor& Color : Colors())
            {
                FCodeCompletionItem* Item = Add(Color.Name, ECodeCompletionKind::Color, "color", {}, {}, 0);
                Item->Swatch = IM_COL32(Color.R, Color.G, Color.B, Color.A);
            }
        }
        for (const FRmlFunction& Function : Functions())
        {
            if ((Function.Kind & Values) != 0)
            {
                Add(Function.Name, ECodeCompletionKind::Function, Function.Signature, Function.Documentation, (FString(Function.Name) + "($0)").View(), (Function.Kind & EValue::Color) ? -1 : 2);
            }
        }
        if (Values & EValue::Tween)
        {
            for (const char* Tween : Tweens())
            {
                Add(Tween, ECodeCompletionKind::Value, "tween", "Easing curve for the transition or animation.", {}, 0);
            }
        }
        if (Values & EValue::Property)
        {
            for (const FRmlProperty& Animated : Properties())
            {
                Add(Animated.Name, ECodeCompletionKind::Property, "property", Animated.Documentation, {}, -1);
            }
        }
        if (IEquals(PropertyName, "animation"))
        {
            TVector<FString> Names;
            CollectAtRuleNames(*Request.Document, "keyframes", Names);
            for (const FString& Name : Names)
            {
                Add(Name.View(), ECodeCompletionKind::Value, "@keyframes", {}, {}, 12);
            }
        }
        if (Values & EValue::Decorator)
        {
            TVector<FString> Names;
            CollectAtRuleNames(*Request.Document, "decorator", Names);
            for (const FString& Name : Names)
            {
                Add(Name.View(), ECodeCompletionKind::Value, "@decorator", {}, {}, 12);
            }
        }
    }

    void FRmlLanguage::CollectSelectors(const FCodeDocument& Document, TVector<FString>& OutClasses, TVector<FString>& OutIds) const
    {
        const FString Text = Document.GetText();
        const FStringView View = Text.View();
        if (bStylesheet)
        {
            ScanStylesheetSelectors(View, OutClasses, OutIds);
            return;
        }

        size_t From = 0;
        while (true)
        {
            const int32 Open = CodeText::FindIgnoreCase(View, "<style", (int32)From);
            if (Open < 0)
            {
                break;
            }
            const size_t BodyStart = View.find('>', Open);
            if (BodyStart == FStringView::npos)
            {
                break;
            }
            const int32 Close = CodeText::FindIgnoreCase(View, "</style", (int32)BodyStart);
            const size_t BodyEnd = Close < 0 ? View.size() : (size_t)Close;
            ScanStylesheetSelectors(View.substr(BodyStart + 1, BodyEnd - BodyStart - 1), OutClasses, OutIds);
            From = BodyEnd;
        }

        if (StylesheetResolver)
        {
            TVector<FString> Links;
            ScanAttributeValues(View, "href", Links);
            for (const FString& Href : Links)
            {
                FString Linked;
                if (Href.View().ends_with(".rcss") && StylesheetResolver(Href.View(), Linked))
                {
                    ScanStylesheetSelectors(Linked.View(), OutClasses, OutIds);
                }
            }
        }

        ScanAttributeValues(View, "class", OutClasses);
        ScanAttributeValues(View, "id", OutIds);
    }

    void FRmlLanguage::CollectAtRuleNames(const FCodeDocument& Document, FStringView AtRule, TVector<FString>& Out) const
    {
        const FString Needle = "@" + FString(AtRule);
        for (int32 Line = 0; Line < Document.GetLineCount(); ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            const int32 Found = CodeText::FindIgnoreCase(Text, Needle.View(), 0);
            if (Found < 0)
            {
                continue;
            }
            int32 Start = Found + (int32)Needle.size();
            while (Start < (int32)Text.size() && CodeText::IsSpace(Text[Start]))
            {
                ++Start;
            }
            const int32 End = SkipIdent(Text, Start, (int32)Text.size());
            AddUnique(Out, Text.substr(Start, End - Start));
        }
    }

    FString FRmlLanguage::FindTagName(const FCodeDocument& Document, FCodePosition Position) const
    {
        for (int32 Line = Position.Line; Line >= Math::Max(0, Position.Line - 40); --Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            int32 Column = Line == Position.Line ? Math::Min(Position.Column, (int32)Text.size()) : (int32)Text.size();
            while (Column > 0)
            {
                --Column;
                if (Text[Column] == '>')
                {
                    return {};
                }
                if (Text[Column] == '<' && Column + 1 < (int32)Text.size() && CodeText::IsAlpha(Text[Column + 1]))
                {
                    int32 End = Column + 1;
                    while (End < (int32)Text.size() && IsNameChar(Text[End]))
                    {
                        ++End;
                    }
                    return FString(Text.substr(Column + 1, End - Column - 1));
                }
            }
        }
        return {};
    }

    void FRmlLanguage::BuildElementStack(const FCodeDocument& Document, FCodePosition Limit, TVector<FOpenElement>& OutStack, TVector<FCodeDiagnostic>* Problems) const
    {
        OutStack.clear();
        if (bStylesheet)
        {
            return;
        }
        FString PendingName;
        FCodePosition PendingPosition;
        bool bPending = false;
        bool bPendingClosing = false;
        const int32 LastLine = Math::Min(Limit.Line, Document.GetLineCount() - 1);
        for (int32 Line = 0; Line <= LastLine; ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            const TVector<ECodeToken>& Tokens = Document.GetTokens(Line);
            if (Tokens.size() != Text.size())
            {
                continue;
            }
            const int32 Size = (int32)Text.size();
            const int32 End = Line == Limit.Line ? Math::Min(Limit.Column, Size) : Size;
            for (int32 Column = 0; Column < End; ++Column)
            {
                if (Tokens[Column] != ECodeToken::Punctuation)
                {
                    continue;
                }
                const char C = Text[Column];
                if (C == '<')
                {
                    const bool bClosing = Column + 1 < Size && Text[Column + 1] == '/';
                    const int32 NameStart = Column + (bClosing ? 2 : 1);
                    int32 NameEnd = NameStart;
                    while (NameEnd < Size && Tokens[NameEnd] == ECodeToken::Tag)
                    {
                        ++NameEnd;
                    }
                    if (NameEnd == NameStart || Text[NameStart] == '!' || Text[NameStart] == '?')
                    {
                        continue;
                    }
                    bPending = true;
                    bPendingClosing = bClosing;
                    PendingName = Text.substr(NameStart, NameEnd - NameStart);
                    PendingPosition = { Line, Column };
                    Column = NameEnd - 1;
                }
                else if (C == '/' && Column + 1 < Size && Text[Column + 1] == '>')
                {
                    bPending = false;
                    ++Column;
                }
                else if (C == '>' && bPending)
                {
                    bPending = false;
                    if (!bPendingClosing)
                    {
                        const FRmlElement* Element = FindElement(PendingName.View());
                        if (Element == nullptr || !Element->bVoid)
                        {
                            OutStack.push_back({ PendingName, PendingPosition });
                        }
                        continue;
                    }
                    int32 Match = -1;
                    for (int32 i = (int32)OutStack.size() - 1; i >= 0 && Match < 0; --i)
                    {
                        Match = EqualsIgnoreCase(OutStack[i].Name.View(), PendingName.View()) ? i : -1;
                    }
                    const FCodeRange ClosingRange{ PendingPosition, { PendingPosition.Line, PendingPosition.Column + 2 + (int32)PendingName.size() } };
                    if (Match < 0)
                    {
                        if (Problems != nullptr)
                        {
                            Problems->push_back({ ClosingRange, Format("</{}> closes nothing, no <{}> is open here", PendingName.c_str(), PendingName.c_str()), ECodeSeverity::Error });
                        }
                        continue;
                    }
                    for (int32 i = (int32)OutStack.size() - 1; i > Match && Problems != nullptr; --i)
                    {
                        const FOpenElement& Open = OutStack[i];
                        const FCodeRange OpenRange{ Open.Position, { Open.Position.Line, Open.Position.Column + 1 + (int32)Open.Name.size() } };
                        Problems->push_back({ OpenRange, Format("<{}> is not closed before </{}>", Open.Name.c_str(), PendingName.c_str()), ECodeSeverity::Warning });
                    }
                    OutStack.resize(Match);
                }
            }
        }
    }

    bool FRmlLanguage::GetDiagnostics(const FCodeDocument& Document, TVector<FCodeDiagnostic>& Out) const
    {
        if (!bStylesheet)
        {
            TVector<FOpenElement> Stack;
            BuildElementStack(Document, Document.GetEnd(), Stack, &Out);
            for (const FOpenElement& Open : Stack)
            {
                const FCodeRange Range{ Open.Position, { Open.Position.Line, Open.Position.Column + 1 + (int32)Open.Name.size() } };
                Out.push_back({ Range, Format("<{}> is never closed", Open.Name.c_str()), ECodeSeverity::Error });
            }
        }

        TVector<bool> DescriptorBlocks;
        bool bDescriptorPending = false;
        const FRmlProperty* CurrentProperty = nullptr;
        for (int32 Line = 0; Line < Document.GetLineCount(); ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            const TVector<ECodeToken>& Tokens = Document.GetTokens(Line);
            if (Tokens.size() != Text.size())
            {
                continue;
            }
            const int32 Size = (int32)Text.size();
            int32 Column = 0;
            while (Column < Size)
            {
                const ECodeToken Token = Tokens[Column];
                int32 End = Column + 1;
                while (End < Size && Tokens[End] == Token && Token != ECodeToken::Punctuation)
                {
                    ++End;
                }
                const FStringView Word = Text.substr(Column, End - Column);
                const FCodeRange Range{ { Line, Column }, { Line, End } };
                switch (Token)
                {
                    case ECodeToken::Keyword:
                        if (Word.starts_with("@"))
                        {
                            const FStringView Rule = Word.substr(1);
                            bDescriptorPending = IEquals(Rule, "spritesheet") || IEquals(Rule, "font-face") || IEquals(Rule, "decorator");
                        }
                        break;
                    case ECodeToken::Punctuation:
                    {
                        const bool bDoubled = (End < Size && Text[End] == Word[0]) || (Column > 0 && Text[Column - 1] == Word[0]);
                        if (Word == "{" && !bDoubled)
                        {
                            DescriptorBlocks.push_back(bDescriptorPending);
                            bDescriptorPending = false;
                            CurrentProperty = nullptr;
                        }
                        else if (Word == "}" && !bDoubled)
                        {
                            if (!DescriptorBlocks.empty())
                            {
                                DescriptorBlocks.pop_back();
                            }
                            CurrentProperty = nullptr;
                        }
                        else if (Word == ";")
                        {
                            CurrentProperty = nullptr;
                        }
                        break;
                    }
                    case ECodeToken::Property:
                    {
                        CurrentProperty = FindProperty(Word);
                        const bool bInDescriptors = !DescriptorBlocks.empty() && DescriptorBlocks.back();
                        if (CurrentProperty == nullptr && !Word.starts_with("--") && !bInDescriptors)
                        {
                            const FRmlProperty* Closest = nullptr;
                            int32 BestDistance = 3;
                            for (const FRmlProperty& Candidate : Properties())
                            {
                                const int32 Distance = EditDistance(Word, Candidate.Name);
                                if (Distance < BestDistance)
                                {
                                    BestDistance = Distance;
                                    Closest = &Candidate;
                                }
                            }
                            FString Message = Format("Unknown property '{}'", FString(Word).c_str());
                            if (Closest != nullptr)
                            {
                                Message += Format(", did you mean '{}'?", Closest->Name);
                            }
                            Out.push_back({ Range, Move(Message), ECodeSeverity::Warning });
                        }
                        break;
                    }
                    case ECodeToken::Number:
                    {
                        uint32 Color = 0;
                        if (Word.starts_with("#") && !CodeLanguageUtil::ParseHexColor(Word, Color))
                        {
                            Out.push_back({ Range, Format("'{}' is not a color, which takes 3, 4, 6 or 8 hex digits", FString(Word).c_str()), ECodeSeverity::Error });
                        }
                        break;
                    }
                    case ECodeToken::Value:
                    {
                        const bool bKeywordOnly = CurrentProperty != nullptr && CurrentProperty->Values == 0 && *CurrentProperty->Keywords != '\0';
                        if (bKeywordOnly && !ContainsWord(CurrentProperty->Keywords, Word) && !IEquals(Word, "inherit") && !IEquals(Word, "initial"))
                        {
                            Out.push_back({ Range, Format("'{}' is not a value of {}, which takes {}", FString(Word).c_str(), CurrentProperty->Name, CurrentProperty->Keywords), ECodeSeverity::Warning });
                        }
                        break;
                    }
                    default:
                        break;
                }
                Column = End;
            }
        }
        return true;
    }

    void FRmlLanguage::GetColorSpans(FStringView Line, const ECodeToken* Tokens, TVector<FCodeColorSpan>& Out) const
    {
        const int32 Size = (int32)Line.size();
        int32 i = 0;
        while (i < Size)
        {
            const ECodeToken Token = Tokens[i];
            int32 End = i + 1;
            while (End < Size && Tokens[End] == Token)
            {
                ++End;
            }
            const FStringView Word = Line.substr(i, End - i);
            uint32 Color = 0;
            if (Token == ECodeToken::Number && Word.starts_with("#") && CodeLanguageUtil::ParseHexColor(Word, Color))
            {
                Out.push_back({ i, End, Color });
            }
            else if (Token == ECodeToken::Value)
            {
                const auto Found = ColorMap.find(Word);
                if (Found != ColorMap.end())
                {
                    Out.push_back({ i, End, Found->second });
                }
            }
            else if (Token == ECodeToken::Function && End < Size && Line[End] == '(')
            {
                const bool bColorFunction = IEquals(Word, "rgb") || IEquals(Word, "rgba") || IEquals(Word, "hsl") || IEquals(Word, "hsla");
                const size_t Close = Line.find(')', End);
                if (bColorFunction && Close != FStringView::npos && ParseColorFunction(Word, Line.substr(End + 1, Close - End - 1), Color))
                {
                    Out.push_back({ i, (int32)Close + 1, Color });
                    End = (int32)Close + 1;
                }
            }
            i = End;
        }
    }

    bool FRmlLanguage::GetHover(const FCodeDocument& Document, FCodePosition Position, FCodeHover& Out) const
    {
        const FStringView Line = Document.GetLineView(Position.Line);
        const TVector<ECodeToken>& Tokens = Document.GetTokens(Position.Line);
        if (Tokens.size() != Line.size() || Position.Column >= (int32)Line.size())
        {
            return false;
        }
        const ECodeToken Token = Tokens[Position.Column];
        int32 Start = Position.Column;
        int32 End = Position.Column + 1;
        while (Start > 0 && Tokens[Start - 1] == Token)
        {
            --Start;
        }
        while (End < (int32)Line.size() && Tokens[End] == Token)
        {
            ++End;
        }
        const FStringView Word = Line.substr(Start, End - Start);
        Out.Range = { { Position.Line, Start }, { Position.Line, End } };

        TVector<FCodeColorSpan> Spans;
        GetColorSpans(Line, Tokens.data(), Spans);
        for (const FCodeColorSpan& Span : Spans)
        {
            if (Span.Start <= Position.Column && Position.Column < Span.End)
            {
                Out.Swatch = Span.Color;
                Out.Title = Line.substr(Span.Start, Span.End - Span.Start);
                Out.Body = DescribeColor(Span.Color) + "\nCtrl+Click to pick a color.";
                return true;
            }
        }

        switch (Token)
        {
            case ECodeToken::Tag:
            {
                const FRmlElement* Element = FindElement(Word);
                if (Element == nullptr)
                {
                    return false;
                }
                Out.Title = "<" + FString(Word) + ">";
                Out.Body = Element->Documentation;
                if (*Element->Attributes != '\0')
                {
                    Out.Body += Format("\n\nAttributes {}", Element->Attributes);
                }
                return true;
            }
            case ECodeToken::Attribute:
            {
                for (const auto& [Name, Description] : RmlBinding::KnownAttributes())
                {
                    const FStringView Known(Name);
                    if (Word == Known || (Known.ends_with("-") && Word.starts_with(Known)))
                    {
                        Out.Title = Word;
                        Out.Body = Description;
                        return true;
                    }
                }
                if (const FRmlNamed* Event = FindNamed(Events(), Word))
                {
                    Out.Title = Word;
                    Out.Body = Event->Documentation;
                    return true;
                }
                const FRmlAttribute* Attribute = FindAttribute(FindTagName(Document, Position).View(), Word);
                if (Attribute == nullptr)
                {
                    return false;
                }
                Out.Title = Word;
                Out.Body = Attribute->Documentation;
                if (*Attribute->Values != '\0')
                {
                    Out.Body += Format("\n\nValues {}", Attribute->Values);
                }
                return true;
            }
            case ECodeToken::Property:
            {
                const FRmlProperty* Property = FindProperty(Word);
                if (Property == nullptr)
                {
                    return false;
                }
                Out.Title = Format("{}: {}", Property->Name, Property->Syntax);
                Out.Body = Property->Documentation;
                if (*Property->Keywords != '\0')
                {
                    Out.Body += Format("\n\nKeywords {}", Property->Keywords);
                }
                return true;
            }
            case ECodeToken::Function:
            {
                const auto Found = FunctionMap.find(Word);
                if (Found == FunctionMap.end())
                {
                    return false;
                }
                Out.Title = Found->second->Signature;
                Out.Body = Found->second->Documentation;
                return true;
            }
            case ECodeToken::Keyword:
            {
                const FRmlNamed* Named = nullptr;
                if (Word.starts_with("@"))
                {
                    Named = FindNamed(AtRules(), Word.substr(1));
                }
                else if (Word.starts_with(":"))
                {
                    Named = FindNamed(PseudoClasses(), Word.substr(Word.starts_with("::") ? 2 : 1));
                }
                else if (Word.starts_with("&"))
                {
                    Named = FindNamed(Entities(), Word.substr(1));
                }
                else if (Word.starts_with("!"))
                {
                    Out.Title = Word;
                    Out.Body = "Wins over any declaration of this property without !important.";
                    return true;
                }
                if (Named == nullptr)
                {
                    return false;
                }
                Out.Title = Word;
                Out.Body = Named->Documentation;
                return true;
            }
            case ECodeToken::Number:
            {
                size_t UnitStart = 0;
                while (UnitStart < Word.size() && (CodeText::IsDigit(Word[UnitStart]) || Word[UnitStart] == '.' || Word[UnitStart] == '-' || Word[UnitStart] == '+'))
                {
                    ++UnitStart;
                }
                const FRmlNamed* Unit = FindNamed(Units(), Word.substr(UnitStart));
                if (Unit == nullptr)
                {
                    return false;
                }
                Out.Title = Word;
                Out.Body = Unit->Documentation;
                return true;
            }
            case ECodeToken::Value:
            {
                for (const char* Tween : Tweens())
                {
                    if (Word == FStringView(Tween))
                    {
                        Out.Title = Word;
                        Out.Body = "Easing curve. The in form eases at the start, out at the end, and in-out at both.";
                        return true;
                    }
                }
                return false;
            }
            case ECodeToken::Binding:
                Out.Title = "Data binding";
                Out.Body = "Expression evaluated against the data model bound by the nearest data-model.";
                return true;
            default:
                return false;
        }
    }

    bool FRmlLanguage::FindSignature(FStringView Function, FCodeSignature& Out) const
    {
        const auto Found = FunctionMap.find(Function);
        if (Found == FunctionMap.end())
        {
            return false;
        }
        Out.Label = Found->second->Signature;
        Out.Documentation = Found->second->Documentation;
        CodeLanguageUtil::ParseSignatureParameters(Out);
        return true;
    }
}
