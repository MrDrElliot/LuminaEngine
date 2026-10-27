#include "CodeDocument.h"

#include "CodeLanguage.h"

namespace Lumina
{
    namespace CodeText
    {
        int32 NextCharStart(FStringView Text, int32 Column)
        {
            const int32 Size = (int32)Text.size();
            if (Column >= Size)
            {
                return Size;
            }
            ++Column;
            while (Column < Size && IsContinuationByte(Text[Column]))
            {
                ++Column;
            }
            return Column;
        }

        int32 PrevCharStart(FStringView Text, int32 Column)
        {
            if (Column <= 0)
            {
                return 0;
            }
            --Column;
            while (Column > 0 && IsContinuationByte(Text[Column]))
            {
                --Column;
            }
            return Column;
        }

        int32 VisualColumn(FStringView Text, int32 Column, int32 TabSize)
        {
            const int32 End = Math::Min(Column, (int32)Text.size());
            int32 Visual = 0;
            for (int32 i = 0; i < End; ++i)
            {
                const char C = Text[i];
                if (C == '\t')
                {
                    Visual += TabSize - (Visual % TabSize);
                }
                else if (!IsContinuationByte(C))
                {
                    ++Visual;
                }
            }
            return Visual;
        }

        int32 ColumnAtVisual(FStringView Text, int32 Visual, int32 TabSize, bool bRoundToNearest)
        {
            int32 Current = 0;
            int32 Column = 0;
            const int32 Size = (int32)Text.size();
            while (Column < Size)
            {
                const int32 Width = Text[Column] == '\t' ? TabSize - (Current % TabSize) : 1;
                const int32 Threshold = bRoundToNearest ? Current + (Width + 1) / 2 : Current + Width;
                if (Visual < Threshold)
                {
                    return Column;
                }
                Current += Width;
                Column = NextCharStart(Text, Column);
            }
            return Size;
        }

        int32 LeadingWhitespace(FStringView Text)
        {
            int32 Count = 0;
            while (Count < (int32)Text.size() && IsSpace(Text[Count]))
            {
                ++Count;
            }
            return Count;
        }

        bool IsBlank(FStringView Text)
        {
            return LeadingWhitespace(Text) == (int32)Text.size();
        }

        bool StartsWithIgnoreCase(FStringView Text, FStringView Prefix)
        {
            return Text.size() >= Prefix.size() && EqualsIgnoreCase(Text.substr(0, Prefix.size()), Prefix);
        }

        int32 FindIgnoreCase(FStringView Haystack, FStringView Needle, int32 From)
        {
            if (Needle.empty())
            {
                return -1;
            }
            const int32 Last = (int32)Haystack.size() - (int32)Needle.size();
            for (int32 i = Math::Max(0, From); i <= Last; ++i)
            {
                if (EqualsIgnoreCase(Haystack.substr(i, Needle.size()), Needle))
                {
                    return i;
                }
            }
            return -1;
        }
    }

    FCodeDocument::FCodeDocument()
    {
        Lines.emplace_back();
    }

    void FCodeDocument::SetText(FStringView Text)
    {
        Lines.clear();
        size_t LineStart = 0;
        for (size_t i = 0; i <= Text.size(); ++i)
        {
            if (i == Text.size() || Text[i] == '\n')
            {
                size_t LineEnd = i;
                if (LineEnd > LineStart && Text[LineEnd - 1] == '\r')
                {
                    --LineEnd;
                }
                FLine& Line = Lines.emplace_back();
                Line.Text.assign(Text.data() + LineStart, LineEnd - LineStart);
                LineStart = i + 1;
            }
        }
        HighlightFrontier = 0;
        ++Version;
    }

    FString FCodeDocument::GetText() const
    {
        FString Result;
        Result.reserve(GetByteSize());
        for (size_t i = 0; i < Lines.size(); ++i)
        {
            if (i > 0)
            {
                Result += '\n';
            }
            Result += Lines[i].Text;
        }
        return Result;
    }

    FString FCodeDocument::GetRangeText(const FCodeRange& Range) const
    {
        const FCodePosition Start = Clamp(Range.Start);
        const FCodePosition End = Clamp(Range.End);
        if (End <= Start)
        {
            return {};
        }
        if (Start.Line == End.Line)
        {
            return FString(GetLineView(Start.Line).substr(Start.Column, End.Column - Start.Column));
        }

        FString Result(GetLineView(Start.Line).substr(Start.Column));
        for (int32 Line = Start.Line + 1; Line < End.Line; ++Line)
        {
            Result += '\n';
            Result += Lines[Line].Text;
        }
        Result += '\n';
        Result += GetLineView(End.Line).substr(0, End.Column);
        return Result;
    }

    size_t FCodeDocument::GetByteSize() const
    {
        size_t Size = Lines.size() - 1;
        for (const FLine& Line : Lines)
        {
            Size += Line.Text.size();
        }
        return Size;
    }

    FCodePosition FCodeDocument::Clamp(FCodePosition Position) const
    {
        if (Position.Line < 0)
        {
            return FCodePosition();
        }
        if (Position.Line >= GetLineCount())
        {
            return GetEnd();
        }
        const FStringView Text = GetLineView(Position.Line);
        Position.Column = Math::Clamp(Position.Column, 0, (int32)Text.size());
        while (Position.Column > 0 && Position.Column < (int32)Text.size() && CodeText::IsContinuationByte(Text[Position.Column]))
        {
            --Position.Column;
        }
        return Position;
    }

    FCodePosition FCodeDocument::GetEnd() const
    {
        const int32 Last = GetLineCount() - 1;
        return FCodePosition{ Last, (int32)Lines[Last].Text.size() };
    }

    FCodePosition FCodeDocument::Next(FCodePosition Position) const
    {
        const FStringView Text = GetLineView(Position.Line);
        if (Position.Column < (int32)Text.size())
        {
            return FCodePosition{ Position.Line, CodeText::NextCharStart(Text, Position.Column) };
        }
        if (Position.Line + 1 < GetLineCount())
        {
            return FCodePosition{ Position.Line + 1, 0 };
        }
        return Position;
    }

    FCodePosition FCodeDocument::Prev(FCodePosition Position) const
    {
        if (Position.Column > 0)
        {
            return FCodePosition{ Position.Line, CodeText::PrevCharStart(GetLineView(Position.Line), Position.Column) };
        }
        if (Position.Line > 0)
        {
            return FCodePosition{ Position.Line - 1, (int32)Lines[Position.Line - 1].Text.size() };
        }
        return Position;
    }

    char FCodeDocument::CharAt(FCodePosition Position) const
    {
        if (Position.Line < 0 || Position.Line >= GetLineCount())
        {
            return 0;
        }
        const FStringView Text = GetLineView(Position.Line);
        if (Position.Column < (int32)Text.size())
        {
            return Text[Position.Column];
        }
        return Position.Line + 1 < GetLineCount() ? '\n' : 0;
    }

    FCodePosition FCodeDocument::Insert(FCodePosition At, FStringView Text)
    {
        At = Clamp(At);
        if (Text.empty())
        {
            return At;
        }

        MarkEdited(At.Line);
        ++Version;

        const size_t FirstBreak = Text.find('\n');
        if (FirstBreak == FStringView::npos)
        {
            Lines[At.Line].Text.insert(At.Column, Text);
            return FCodePosition{ At.Line, At.Column + (int32)Text.size() };
        }

        // Every new line is built first and spliced in once, so a large paste moves the tail of the document a single time.
        TVector<FLine> Inserted;
        size_t SegmentStart = FirstBreak + 1;
        for (size_t i = SegmentStart; i <= Text.size(); ++i)
        {
            if (i < Text.size() && Text[i] != '\n')
            {
                continue;
            }
            const size_t SegmentEnd = (i > SegmentStart && Text[i - 1] == '\r') ? i - 1 : i;
            Inserted.emplace_back().Text.assign(Text.data() + SegmentStart, SegmentEnd - SegmentStart);
            SegmentStart = i + 1;
        }

        FString& First = Lines[At.Line].Text;
        FLine& Last = Inserted.back();
        const FCodePosition End{ At.Line + (int32)Inserted.size(), (int32)Last.Text.size() };
        Last.Text += First.View().substr(At.Column);
        const size_t FirstEnd = (FirstBreak > 0 && Text[FirstBreak - 1] == '\r') ? FirstBreak - 1 : FirstBreak;
        First.resize(At.Column);
        First.append(Text.data(), FirstEnd);

        Lines.insert(Lines.begin() + At.Line + 1, std::make_move_iterator(Inserted.begin()), std::make_move_iterator(Inserted.end()));
        return End;
    }

    void FCodeDocument::Erase(const FCodeRange& Range)
    {
        const FCodePosition Start = Clamp(Range.Start);
        const FCodePosition End = Clamp(Range.End);
        if (End <= Start)
        {
            return;
        }

        FString& First = Lines[Start.Line].Text;
        if (Start.Line == End.Line)
        {
            First.erase(Start.Column, End.Column - Start.Column);
        }
        else
        {
            const FString Tail(GetLineView(End.Line).substr(End.Column));
            First.resize(Start.Column);
            First += Tail;
            Lines.erase(Lines.begin() + Start.Line + 1, Lines.begin() + End.Line + 1);
        }
        MarkEdited(Start.Line);
        ++Version;
    }

    size_t FCodeDocument::PositionToOffset(FCodePosition Position) const
    {
        Position = Clamp(Position);
        size_t Offset = 0;
        for (int32 Line = 0; Line < Position.Line; ++Line)
        {
            Offset += Lines[Line].Text.size() + 1;
        }
        return Offset + Position.Column;
    }

    FCodePosition FCodeDocument::OffsetToPosition(size_t Offset) const
    {
        for (int32 Line = 0; Line < GetLineCount(); ++Line)
        {
            const size_t Size = Lines[Line].Text.size();
            if (Offset <= Size)
            {
                return Clamp(FCodePosition{ Line, (int32)Offset });
            }
            Offset -= Size + 1;
        }
        return GetEnd();
    }

    void FCodeDocument::MarkEdited(int32 Line)
    {
        Lines[Line].bDirty = true;
        HighlightFrontier = Math::Min(HighlightFrontier, Line);
    }

    void FCodeDocument::InvalidateHighlight()
    {
        for (FLine& Line : Lines)
        {
            Line.bDirty = true;
        }
        HighlightFrontier = 0;
    }

    void FCodeDocument::Highlight(const FCodeLanguage& Language, int32 Line)
    {
        Line = Math::Min(Line, GetLineCount() - 1);
        for (int32 Index = HighlightFrontier; Index <= Line; ++Index)
        {
            FLine& Current = Lines[Index];
            const uint32 StartState = Index == 0 ? Language.GetInitialState() : Lines[Index - 1].EndState;
            if (Current.bDirty || Current.StartState != StartState || Current.Tokens.size() != Current.Text.size())
            {
                Current.Tokens.resize(Current.Text.size());
                Current.StartState = StartState;
                Current.EndState = Language.Tokenize(Current.Text.View(), StartState, Current.Tokens.data());
                Current.bDirty = false;
            }
        }
        HighlightFrontier = Math::Max(HighlightFrontier, Line + 1);
    }

    uint32 FCodeDocument::GetStartState(const FCodeLanguage& Language, int32 Line) const
    {
        return Line <= 0 ? Language.GetInitialState() : Lines[Line - 1].EndState;
    }

    ECodeToken FCodeDocument::GetTokenAt(FCodePosition Position) const
    {
        if (Position.Line < 0 || Position.Line >= GetLineCount())
        {
            return ECodeToken::Default;
        }
        const TVector<ECodeToken>& Tokens = Lines[Position.Line].Tokens;
        if (Position.Column < 0 || Position.Column >= (int32)Tokens.size())
        {
            return ECodeToken::Default;
        }
        return Tokens[Position.Column];
    }
}
