#include "ClangParserContext.h"

#include <iostream>

#include "Utils.h"
#include "xxhash.h"
#include <queue>

namespace Lumina::Reflection
{
    void FClangParserContext::AddReflectedMacro(FReflectionMacro&& Macro)
    {
        uint64_t Hash = XXH64(Macro.HeaderID.c_str(), strlen(Macro.HeaderID.c_str()), 0);

        std::vector<FReflectionMacro>& Macros = ReflectionMacros[Hash];
        Macros.push_back(std::move(Macro));
    }

    void FClangParserContext::AddGeneratedBodyMacro(FReflectionMacro&& Macro)
    {
        uint64_t Hash = ClangUtils::HashString(Macro.HeaderID);
        
        std::queue<FReflectionMacro>& Macros = GeneratedBodyMacros[Hash];
        Macros.push(std::move(Macro));
    }

    bool FClangParserContext::TryFindMacroForCursor(const std::string& HeaderID, const CXCursor& Cursor, FReflectionMacro& Macro, bool bConsume)
    {
        uint64_t Hash = ClangUtils::HashString(HeaderID);

        auto HeaderIter = ReflectionMacros.find(Hash);
        if (HeaderIter == ReflectionMacros.end())
        {
            return false;
        }

        CXSourceRange typeRange = clang_getCursorExtent(Cursor);
        CXSourceLocation startLoc = clang_getRangeStart(typeRange);

        CXFile cursorFile;
        uint32_t cursorLine, cursorColumn;
        clang_getExpansionLocation(startLoc, &cursorFile, &cursorLine, &cursorColumn, nullptr);

        // Position is monotonic with source order, used as a tiebreaker when macro and cursor share a line.
        const int32_t cursorPosition = (int32_t)typeRange.begin_int_data;

        CXString FileName = clang_getFileName(cursorFile);

        if (FileName.data == nullptr)
        {
            return false;
        }

        std::string FileNameChar = clang_getCString(FileName);
        clang_disposeString(FileName);

        // Normalized the same way HeaderID was, so case-sensitive filesystems do not drop legitimate hits.
        FileNameChar = ClangUtils::NormalizeHeaderPath(std::move(FileNameChar));

        if (FileNameChar != HeaderID)
        {
            return false;
        }

        std::vector<FReflectionMacro>& MacrosForHeader = HeaderIter->second;

        // Matched on the macro's closing line, so a wrapped argument list still binds to the field below it.
        // Without the same-line case, inline-form macros mis-bind to the cursor below them.
        auto SameLineMatch = MacrosForHeader.end();
        auto LineAboveMatch = MacrosForHeader.end();

        for (auto iter = MacrosForHeader.begin(); iter != MacrosForHeader.end(); ++iter)
        {
            if (iter->EndLineNumber == cursorLine && iter->Position < cursorPosition)
            {
                if (SameLineMatch == MacrosForHeader.end() || iter->Position > SameLineMatch->Position)
                {
                    SameLineMatch = iter;
                }
            }
            else if (iter->EndLineNumber + 1 == cursorLine)
            {
                if (LineAboveMatch == MacrosForHeader.end() || iter->Position > LineAboveMatch->Position)
                {
                    LineAboveMatch = iter;
                }
            }
        }

        auto Best = (SameLineMatch != MacrosForHeader.end()) ? SameLineMatch : LineAboveMatch;
        if (Best != MacrosForHeader.end())
        {
            Macro = *Best;
            if (bConsume)
            {
                MacrosForHeader.erase(Best);
            }
            return true;
        }

        return false;
    }

    bool FClangParserContext::TryFindGeneratedBodyMacro(const std::string& HeaderID, const CXCursor& Cursor, FReflectionMacro& Macro)
    {
        // A pure lookup, since the struct visitor decides what a missing GENERATED_BODY means.
        uint64_t Hash = XXH64(HeaderID.c_str(), strlen(HeaderID.c_str()), 0);
        auto headerIter = GeneratedBodyMacros.find(Hash);
        if (headerIter == GeneratedBodyMacros.end())
        {
            Macro = {};
            return false;
        }

        
        std::queue<FReflectionMacro>& MacrosForHeader = headerIter->second;

        if (MacrosForHeader.empty())
        {
            Macro = {};
            return false;
        }
        
        Macro = MacrosForHeader.front();
        
        MacrosForHeader.pop();

        return true;
    }
    
    // Joined with a scope separator, since concatenating bare turned MyGame::Editor into MyGameEditor.
    void FClangParserContext::RebuildCurrentNamespace()
    {
        CurrentNamespace.clear();

        for (const std::string& Segment : NamespaceStack)
        {
            if (!CurrentNamespace.empty())
            {
                CurrentNamespace.append("::");
            }

            CurrentNamespace.append(Segment);
        }
    }

    void FClangParserContext::PushNamespace(const std::string& Namespace)
    {
        NamespaceStack.push_back(Namespace);

        RebuildCurrentNamespace();
    }

    void FClangParserContext::PopNamespace()
    {
        NamespaceStack.pop_back();

        RebuildCurrentNamespace();
    }
}
