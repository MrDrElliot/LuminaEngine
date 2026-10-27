#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"

namespace Lumina
{
    class FCodeLanguage;

    struct FCodePosition
    {
        int32 Line = 0;
        // A byte offset into the line's UTF-8 text, always on a character boundary.
        int32 Column = 0;

        auto operator<=>(const FCodePosition&) const = default;
        bool operator==(const FCodePosition&) const = default;
    };

    struct FCodeRange
    {
        FCodePosition Start;
        FCodePosition End;

        bool IsEmpty() const { return Start == End; }
        bool Contains(FCodePosition Position) const { return Start <= Position && Position < End; }
        bool operator==(const FCodeRange&) const = default;
    };

    enum class ECodeToken : uint8
    {
        Default,
        Keyword,
        Type,
        Function,
        KnownIdentifier,
        Number,
        String,
        Comment,
        Punctuation,
        Preprocessor,
        Tag,
        Attribute,
        Property,
        Selector,
        Value,
        Binding,

        Count,
    };

    namespace CodeText
    {
        inline bool IsContinuationByte(char C) { return ((uint8)C & 0xC0) == 0x80; }
        inline bool IsSpace(char C) { return C == ' ' || C == '\t'; }
        inline bool IsDigit(char C) { return C >= '0' && C <= '9'; }
        inline bool IsAlpha(char C) { return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z'); }
        inline bool IsHexDigit(char C) { return IsDigit(C) || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F'); }
        inline char ToLower(char C) { return (C >= 'A' && C <= 'Z') ? (char)(C - 'A' + 'a') : C; }
        inline char ToUpper(char C) { return (C >= 'a' && C <= 'z') ? (char)(C - 'a' + 'A') : C; }

        EDITOR_API int32 NextCharStart(FStringView Text, int32 Column);
        EDITOR_API int32 PrevCharStart(FStringView Text, int32 Column);

        // Tabs advance to the next stop and every code point takes one cell.
        EDITOR_API int32 VisualColumn(FStringView Text, int32 Column, int32 TabSize);
        EDITOR_API int32 ColumnAtVisual(FStringView Text, int32 Visual, int32 TabSize, bool bRoundToNearest);

        EDITOR_API int32 LeadingWhitespace(FStringView Text);
        EDITOR_API bool  IsBlank(FStringView Text);
        EDITOR_API bool  StartsWithIgnoreCase(FStringView Text, FStringView Prefix);
        EDITOR_API int32 FindIgnoreCase(FStringView Haystack, FStringView Needle, int32 From);
    }

    class EDITOR_API FCodeDocument
    {
    public:

        FCodeDocument();

        void SetText(FStringView Text);
        FString GetText() const;
        FString GetRangeText(const FCodeRange& Range) const;
        size_t GetByteSize() const;

        int32 GetLineCount() const { return (int32)Lines.size(); }
        const FString& GetLine(int32 Line) const { return Lines[Line].Text; }
        FStringView GetLineView(int32 Line) const { return Lines[Line].Text.View(); }

        FCodePosition Clamp(FCodePosition Position) const;
        FCodePosition GetEnd() const;
        FCodePosition Next(FCodePosition Position) const;
        FCodePosition Prev(FCodePosition Position) const;

        // A newline at the end of a line and zero past the end of the document.
        char CharAt(FCodePosition Position) const;

        // Returns the position just past the inserted text.
        FCodePosition Insert(FCodePosition At, FStringView Text);
        void Erase(const FCodeRange& Range);

        size_t PositionToOffset(FCodePosition Position) const;
        FCodePosition OffsetToPosition(size_t Offset) const;

        uint64 GetVersion() const { return Version; }

        // Tokenizes lazily up to Line, resuming from the first line an edit touched.
        void Highlight(const FCodeLanguage& Language, int32 Line);
        void InvalidateHighlight();
        const TVector<ECodeToken>& GetTokens(int32 Line) const { return Lines[Line].Tokens; }
        uint32 GetStartState(const FCodeLanguage& Language, int32 Line) const;
        ECodeToken GetTokenAt(FCodePosition Position) const;

    private:

        struct FLine
        {
            FString             Text;
            TVector<ECodeToken> Tokens;
            uint32              StartState = 0;
            uint32              EndState = 0;
            bool                bDirty = true;
        };

        void MarkEdited(int32 Line);

        TVector<FLine> Lines;
        int32          HighlightFrontier = 0;
        uint64         Version = 0;
    };
}
