#include "ReflectionMacro.h"

#include "Reflector/Clang/Utils.h"


namespace Lumina::Reflection
{
    FReflectionMacro::FReflectionMacro(const std::string& HeaderPath, const CXCursor& Cursor, const CXSourceRange& Range, EReflectionMacro InType)
        : Type(InType)
        , HeaderID(HeaderPath)
        , Position(Range.begin_int_data)
    {
        clang_getExpansionLocation(clang_getRangeStart(Range), nullptr, &LineNumber, nullptr, nullptr);
        
        CXToken* Tokens = nullptr;
        CXTranslationUnit TranslationUnit = clang_Cursor_getTranslationUnit(Cursor);
        
        uint32_t NumTokens = 0;
        clang_tokenize(TranslationUnit, Range, &Tokens, &NumTokens);
        for (uint32_t n = 0; n < NumTokens; n++)
        {
            MacroContents += ClangUtils::GetString(clang_getTokenSpelling(TranslationUnit, Tokens[n]));
        }
        clang_disposeTokens(TranslationUnit, Tokens, NumTokens);

        const size_t StartIdx = MacroContents.find_first_of("(");
        const size_t EndIdx = MacroContents.find_last_of(')');
        if (StartIdx != std::string::npos && EndIdx != std::string::npos && EndIdx > StartIdx)
        {
            MacroContents = MacroContents.substr(StartIdx + 1, EndIdx - StartIdx - 1);
        }
        else
        {
            MacroContents.clear();
        }
    }
}
