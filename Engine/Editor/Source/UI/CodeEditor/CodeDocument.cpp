#include "CodeDocument.h"

#include "CodeLanguage.h"

namespace Lumina
{
    namespace CodeText
    {
        int32 NextCharStart(FStringView Text, int32 Column)
        {
            const int32 Size = Text.Len();
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
            const int32 End = Math::Min(Column, Text.Len());
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
            const int32 Size = Text.Len();
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
            while (Count < Text.Len() && IsSpace(Text[Count]))
            {
                ++Count;
            }
            return Count;
        }

        bool IsBlank(FStringView Text)
        {
            return LeadingWhitespace(Text) == Text.Len();
        }

        bool StartsWithIgnoreCase(FStringView Text, FStringView Prefix)
        {
            return Text.Len() >= Prefix.Len() && EqualsIgnoreCase(Text.Mid(0, Prefix.Len()), Prefix);
        }

        int32 FindIgnoreCase(FStringView Haystack, FStringView Needle, int32 From)
        {
            if (Needle.IsEmpty())
            {
                return -1;
            }
            const int32 Last = Haystack.Len() - Needle.Len();
            for (int32 i = Math::Max(0, From); i <= Last; ++i)
            {
                if (EqualsIgnoreCase(Haystack.Mid(i, Needle.Len()), Needle))
                {
                    return i;
                }
            }
            return -1;
        }
    }

    FCodeDocument::FCodeDocument()
    {
        Lines.Emplace();
    }

    void FCodeDocument::SetText(FStringView Text)
    {
        Lines.Reset();
        int32 LineStart = 0;
        for (int32 i = 0; i <= Text.Len(); ++i)
        {
            if (i == Text.Len() || Text[i] == '\n')
            {
                int32 LineEnd = i;
                if (LineEnd > LineStart && Text[LineEnd - 1] == '\r')
                {
                    --LineEnd;
                }
                FLine& Line = Lines.EmplaceGetRef();
                Line.Text = FStringView(Text.GetData() + LineStart, LineEnd - LineStart);
                LineStart = i + 1;
            }
        }
        HighlightFrontier = 0;
        ++Version;
    }

    FString FCodeDocument::GetText() const
    {
        FString Result;
        Result.Reserve(IntCastChecked<int32>(GetByteSize()));
        for (int32 i = 0; i < Lines.Num(); ++i)
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
            return FString(GetLineView(Start.Line).Mid(Start.Column, End.Column - Start.Column));
        }

        FString Result(GetLineView(Start.Line).Mid(Start.Column));
        for (int32 Line = Start.Line + 1; Line < End.Line; ++Line)
        {
            Result += '\n';
            Result += Lines[Line].Text;
        }
        Result += '\n';
        Result += GetLineView(End.Line).Mid(0, End.Column);
        return Result;
    }

    size_t FCodeDocument::GetByteSize() const
    {
        int32 Size = Lines.Num() - 1;
        for (const FLine& Line : Lines)
        {
            Size += Line.Text.Len();
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
        Position.Column = Math::Clamp(Position.Column, 0, Text.Len());
        while (Position.Column > 0 && Position.Column < Text.Len() && CodeText::IsContinuationByte(Text[Position.Column]))
        {
            --Position.Column;
        }
        return Position;
    }

    FCodePosition FCodeDocument::GetEnd() const
    {
        const int32 Last = GetLineCount() - 1;
        return FCodePosition{ Last, Lines[Last].Text.Len() };
    }

    FCodePosition FCodeDocument::Next(FCodePosition Position) const
    {
        const FStringView Text = GetLineView(Position.Line);
        if (Position.Column < Text.Len())
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
            return FCodePosition{ Position.Line - 1, Lines[Position.Line - 1].Text.Len() };
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
        if (Position.Column < Text.Len())
        {
            return Text[Position.Column];
        }
        return Position.Line + 1 < GetLineCount() ? '\n' : 0;
    }

    FCodePosition FCodeDocument::Insert(FCodePosition At, FStringView Text)
    {
        At = Clamp(At);
        if (Text.IsEmpty())
        {
            return At;
        }

        MarkEdited(At.Line);
        ++Version;

        const int32 FirstBreak = Text.Find('\n');
        if (FirstBreak == Constants::kIndexNone)
        {
            Lines[At.Line].Text.InsertAt(At.Column, Text);
            return FCodePosition{ At.Line, At.Column + Text.Len() };
        }

        // Every new line is built first and spliced in once, so a large paste moves the tail of the document a single time.
        TVector<FLine> Inserted;
        int32 SegmentStart = FirstBreak + 1;
        for (int32 i = SegmentStart; i <= Text.Len(); ++i)
        {
            if (i < Text.Len() && Text[i] != '\n')
            {
                continue;
            }
            const size_t SegmentEnd = (i > SegmentStart && Text[i - 1] == '\r') ? i - 1 : i;
            Inserted.EmplaceGetRef().Text = FStringView(Text.GetData() + SegmentStart, IntCastChecked<int32>(SegmentEnd - SegmentStart));
            SegmentStart = i + 1;
        }

        FString& First = Lines[At.Line].Text;
        FLine& Last = Inserted.Last();
        const FCodePosition End{ At.Line + Inserted.Num(), Last.Text.Len() };
        Last.Text += First.View().Mid(At.Column);
        const size_t FirstEnd = (FirstBreak > 0 && Text[FirstBreak - 1] == '\r') ? FirstBreak - 1 : FirstBreak;
        First.SetLen(At.Column);
        First.Append(Text.GetData(), IntCastChecked<int32>(FirstEnd));

        const int32 SpliceAt = At.Line + 1;
        Lines.InsertDefaulted(SpliceAt, Inserted.Num());
        for (int32 i = 0; i < Inserted.Num(); ++i)
        {
            Lines[SpliceAt + i] = Move(Inserted[i]);
        }
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
            First.RemoveAt(Start.Column, End.Column - Start.Column);
        }
        else
        {
            const FString Tail(GetLineView(End.Line).Mid(End.Column));
            First.SetLen(Start.Column);
            First += Tail;
            Lines.RemoveAt(Start.Line + 1, End.Line - Start.Line);
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
            Offset += Lines[Line].Text.Len() + 1;
        }
        return Offset + Position.Column;
    }

    FCodePosition FCodeDocument::OffsetToPosition(size_t Offset) const
    {
        for (int32 Line = 0; Line < GetLineCount(); ++Line)
        {
            const int32 Size = Lines[Line].Text.Len();
            if (Offset <= static_cast<size_t>(Size))
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
            if (Current.bDirty || Current.StartState != StartState || Current.Tokens.Num() != Current.Text.Len())
            {
                Current.Tokens.SetNum(Current.Text.Len());
                Current.StartState = StartState;
                Current.EndState = Language.Tokenize(Current.Text.View(), StartState, Current.Tokens.GetData());
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
        if (Position.Column < 0 || Position.Column >= Tokens.Num())
        {
            return ECodeToken::Default;
        }
        return Tokens[Position.Column];
    }
}
