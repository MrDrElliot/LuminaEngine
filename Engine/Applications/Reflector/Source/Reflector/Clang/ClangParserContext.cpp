#include "ClangParserContext.h"

#include <limits>
#include "Utils.h"

namespace Lumina::Reflection
{
    void FMacroPool::Add(FReflectionMacro&& Macro)
    {
        const uint32_t EndLine = Macro.EndLineNumber;
        Macros.push_back(std::move(Macro));
        ByEndLine[EndLine].push_back(static_cast<uint32_t>(Macros.size() - 1));
    }

    FReflectionMacro* FMacroPool::FindOnLine(uint32_t Line, int32_t BeforePosition)
    {
        const auto Bucket = ByEndLine.find(Line);
        if (Bucket == ByEndLine.end())
        {
            return nullptr;
        }

        FReflectionMacro* Best = nullptr;
        for (const uint32_t Index : Bucket->second)
        {
            FReflectionMacro& Candidate = Macros[Index];
            if (Candidate.bConsumed || Candidate.Position >= BeforePosition)
            {
                continue;
            }

            if (Best == nullptr || Candidate.Position > Best->Position)
            {
                Best = &Candidate;
            }
        }

        return Best;
    }

    void FClangParserContext::AddReflectedMacro(FReflectionMacro&& Macro)
    {
        ReflectionMacros[Macro.Header].Add(std::move(Macro));
    }

    void FClangParserContext::AddGeneratedBodyMacro(FReflectionMacro&& Macro)
    {
        GeneratedBodyMacros[Macro.Header].Push(std::move(Macro));
    }

    FReflectedHeader* FClangParserContext::ResolveHeaderForFile(CXFile File)
    {
        if (File == nullptr)
        {
            return nullptr;
        }

        const auto [Entry, bIsNew] = FileHeaders.try_emplace(File, nullptr);
        if (bIsNew)
        {
            const std::string Path = ClangUtils::NormalizeHeaderPath(
                ClangUtils::GetString(clang_getFileName(File)));

            const auto Known = AllHeaders.find(FStringHash(Path));
            Entry->second = Known != AllHeaders.end() ? Known->second : nullptr;
        }

        return Entry->second;
    }

    FReflectedHeader* FClangParserContext::ResolveHeaderForCursor(const CXCursor& Cursor)
    {
        CXFile File = nullptr;
        clang_getExpansionLocation(clang_getRangeStart(clang_getCursorExtent(Cursor)), &File, nullptr, nullptr, nullptr);
        return ResolveHeaderForFile(File);
    }

    bool FClangParserContext::TryFindMacroForCursor(const FReflectedHeader* Header, const CXCursor& Cursor, FReflectionMacro& Macro, bool bConsume)
    {
        const auto HeaderIter = ReflectionMacros.find(Header);
        if (HeaderIter == ReflectionMacros.end())
        {
            return false;
        }

        const CXSourceRange TypeRange = clang_getCursorExtent(Cursor);

        CXFile CursorFile = nullptr;
        uint32_t CursorLine = 0;
        clang_getExpansionLocation(clang_getRangeStart(TypeRange), &CursorFile, &CursorLine, nullptr, nullptr);

        if (ResolveHeaderForFile(CursorFile) != Header)
        {
            return false;
        }

        // Position is monotonic with source order, used as a tiebreaker when macro and cursor share a line.
        const int32_t CursorPosition = (int32_t)TypeRange.begin_int_data;

        FMacroPool& Pool = HeaderIter->second;

        // The same-line case first, or an inline-form macro mis-binds to the cursor below it.
        FReflectionMacro* Best = Pool.FindOnLine(CursorLine, CursorPosition);
        if (Best == nullptr && CursorLine > 0)
        {
            Best = Pool.FindOnLine(CursorLine - 1, std::numeric_limits<int32_t>::max());
        }

        if (Best == nullptr)
        {
            return false;
        }

        Macro = *Best;
        if (bConsume)
        {
            Best->bConsumed = true;
        }
        return true;
    }

    bool FClangParserContext::TryFindGeneratedBodyMacro(const FReflectedHeader* Header, const CXCursor&, FReflectionMacro& Macro)
    {
        // A pure lookup, since the struct visitor decides what a missing GENERATED_BODY means.
        const auto HeaderIter = GeneratedBodyMacros.find(Header);
        if (HeaderIter == GeneratedBodyMacros.end())
        {
            Macro = {};
            return false;
        }

        const FReflectionMacro* Next = HeaderIter->second.Pop();
        if (Next == nullptr)
        {
            Macro = {};
            return false;
        }

        Macro = *Next;
        return true;
    }

    // Joined with a scope separator, since concatenating bare turned MyGame::Editor into MyGameEditor.
    void FClangParserContext::PushNamespace(std::string_view Namespace)
    {
        NamespaceLengths.push_back(CurrentNamespace.size());

        if (!CurrentNamespace.empty())
        {
            CurrentNamespace.append("::");
        }

        CurrentNamespace.append(Namespace);
    }

    void FClangParserContext::PopNamespace()
    {
        CurrentNamespace.resize(NamespaceLengths.back());
        NamespaceLengths.pop_back();
    }
}
