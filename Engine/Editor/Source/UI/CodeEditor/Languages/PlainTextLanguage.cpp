#include "PlainTextLanguage.h"

namespace Lumina
{
    uint32 FPlainTextLanguage::Tokenize(FStringView Line, uint32 State, ECodeToken* OutTokens) const
    {
        const int32 Size = (int32)Line.size();
        int32 Column = 0;
        while (Column < Size)
        {
            const char C = Line[Column];
            if (!LineComment.empty() && Line.substr(Column).starts_with(LineComment))
            {
                for (; Column < Size; ++Column)
                {
                    OutTokens[Column] = ECodeToken::Comment;
                }
                break;
            }
            if (C == '"')
            {
                const int32 Start = Column++;
                while (Column < Size && Line[Column] != '"')
                {
                    Column += Line[Column] == '\\' ? 2 : 1;
                }
                Column = Math::Min(Column + 1, Size);
                for (int32 Index = Start; Index < Column; ++Index)
                {
                    OutTokens[Index] = ECodeToken::String;
                }
                continue;
            }
            if (CodeText::IsDigit(C) && (Column == 0 || !IsWordChar(Line[Column - 1])))
            {
                while (Column < Size && (IsWordChar(Line[Column]) || Line[Column] == '.'))
                {
                    OutTokens[Column++] = ECodeToken::Number;
                }
                continue;
            }
            const bool bBracket = C == '{' || C == '}' || C == '[' || C == ']' || C == '(' || C == ')' || C == ':' || C == ',';
            OutTokens[Column++] = bBracket ? ECodeToken::Punctuation : ECodeToken::Default;
        }
        return State;
    }
}
