#include "ReflectionMacro.h"

#include "Reflector/Clang/Utils.h"


namespace Lumina::Reflection
{
    FReflectionMacro::FReflectionMacro(const FReflectedHeader* InHeader, const CXCursor& Cursor, const CXSourceRange& Range, EReflectionMacro InType)
        : Type(InType)
        , Header(InHeader)
        , Position(Range.begin_int_data)
    {
        clang_getExpansionLocation(clang_getRangeStart(Range), nullptr, &LineNumber, nullptr, nullptr);
        clang_getExpansionLocation(clang_getRangeEnd(Range), nullptr, &EndLineNumber, nullptr, nullptr);

        CXToken* Tokens = nullptr;
        CXTranslationUnit TranslationUnit = clang_Cursor_getTranslationUnit(Cursor);

        uint32_t NumTokens = 0;
        clang_tokenize(TranslationUnit, Range, &Tokens, &NumTokens);
        for (uint32_t n = 0; n < NumTokens; n++)
        {
            const CXString Spelling = clang_getTokenSpelling(TranslationUnit, Tokens[n]);
            if (const char* Text = clang_getCString(Spelling))
            {
                MacroContents.append(Text);
            }
            clang_disposeString(Spelling);
        }
        clang_disposeTokens(TranslationUnit, Tokens, NumTokens);

        const size_t StartIdx = MacroContents.find_first_of("(");
        const size_t EndIdx = MacroContents.find_last_of(')');
        if (StartIdx != std::string::npos && EndIdx != std::string::npos && EndIdx > StartIdx)
        {
            MacroContents.erase(EndIdx);
            MacroContents.erase(0, StartIdx + 1);
        }
        else
        {
            MacroContents.clear();
        }
    }
}
