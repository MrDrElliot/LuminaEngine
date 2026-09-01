#include "ReflectionSpecifiers.h"

#include <algorithm>
#include <array>
#include <ranges>
#include "Reflector/Diagnostics/LRTDiagnostics.h"

namespace Lumina::Reflection
{
    namespace
    {
#define LUMINA_SPECIFIER_ROW(Name, Form, Consumer, Doc) \
    { #Name, ESpecifierForm::Form, ESpecifierConsumer::Consumer, Doc },

        // Sorted during constant evaluation so every lookup is a binary search over a read-only table.
        template<typename TTable>
        constexpr TTable SortedByName(TTable Table)
        {
            std::ranges::sort(Table, {}, &FSpecifierInfo::Name);
            return Table;
        }

        constexpr auto GReflectSpecifiers = SortedByName(
            std::to_array<FSpecifierInfo>({ LUMINA_REFLECT_SPECIFIERS(LUMINA_SPECIFIER_ROW) }));
        constexpr auto GPropertySpecifiers = SortedByName(
            std::to_array<FSpecifierInfo>({ LUMINA_PROPERTY_SPECIFIERS(LUMINA_SPECIFIER_ROW) }));
        constexpr auto GFunctionSpecifiers = SortedByName(
            std::to_array<FSpecifierInfo>({ LUMINA_FUNCTION_SPECIFIERS(LUMINA_SPECIFIER_ROW) }));
        constexpr auto GScriptExportSpecifiers = SortedByName(
            std::to_array<FSpecifierInfo>({ LUMINA_SCRIPT_EXPORT_SPECIFIERS(LUMINA_SPECIFIER_ROW) }));

#undef LUMINA_SPECIFIER_ROW

        // A duplicate row would make one of the two spellings unreachable through the binary search.
        static_assert(std::ranges::adjacent_find(GReflectSpecifiers, {}, &FSpecifierInfo::Name) == GReflectSpecifiers.end());
        static_assert(std::ranges::adjacent_find(GPropertySpecifiers, {}, &FSpecifierInfo::Name) == GPropertySpecifiers.end());
        static_assert(std::ranges::adjacent_find(GFunctionSpecifiers, {}, &FSpecifierInfo::Name) == GFunctionSpecifiers.end());
        static_assert(std::ranges::adjacent_find(GScriptExportSpecifiers, {}, &FSpecifierInfo::Name) == GScriptExportSpecifiers.end());

        constexpr size_t kMaxSpecifierLength = 64;

        // Only reached for a specifier that already failed lookup, so one stack row beats a heap allocation.
        uint32_t EditDistance(std::string_view A, std::string_view B)
        {
            if (B.size() >= kMaxSpecifierLength)
            {
                return kMaxSpecifierLength;
            }

            uint32_t Row[kMaxSpecifierLength + 1];
            for (uint32_t J = 0; J <= B.size(); ++J)
            {
                Row[J] = J;
            }

            for (uint32_t I = 1; I <= A.size(); ++I)
            {
                uint32_t Diagonal = Row[0];
                Row[0] = I;

                for (uint32_t J = 1; J <= B.size(); ++J)
                {
                    const uint32_t Previous = Row[J];
                    const uint32_t Cost = (A[I - 1] == B[J - 1]) ? 0u : 1u;
                    Row[J] = std::min(std::min(Row[J] + 1, Row[J - 1] + 1), Diagonal + Cost);
                    Diagonal = Previous;
                }
            }

            return Row[B.size()];
        }
    }

    const char* SpecifierTargetToString(ESpecifierTarget Target)
    {
        switch (Target)
        {
            case ESpecifierTarget::Reflect:      return "REFLECT";
            case ESpecifierTarget::Property:     return "PROPERTY";
            case ESpecifierTarget::Function:     return "FUNCTION";
            case ESpecifierTarget::ScriptExport: return "SCRIPT_EXPORT";
            default:                             return "NONE";
        }
    }

    std::span<const FSpecifierInfo> GetSpecifiers(ESpecifierTarget Target)
    {
        switch (Target)
        {
            case ESpecifierTarget::Reflect:      return GReflectSpecifiers;
            case ESpecifierTarget::Property:     return GPropertySpecifiers;
            case ESpecifierTarget::Function:     return GFunctionSpecifiers;
            case ESpecifierTarget::ScriptExport: return GScriptExportSpecifiers;
            default:                             return {};
        }
    }

    const FSpecifierInfo* FindSpecifier(ESpecifierTarget Target, std::string_view Key)
    {
        const std::span<const FSpecifierInfo> Table = GetSpecifiers(Target);

        const auto Found = std::ranges::lower_bound(Table, Key, {}, &FSpecifierInfo::Name);
        return Found != Table.end() && Found->Name == Key ? &*Found : nullptr;
    }

    const FSpecifierInfo* SuggestSpecifier(ESpecifierTarget Target, std::string_view Key)
    {
        // Loose enough to catch a plausible misspelling, tight enough that unrelated names never match.
        const uint32_t MaxDistance = Key.length() <= 4 ? 1u : (Key.length() <= 8 ? 2u : 3u);

        const FSpecifierInfo* Best = nullptr;
        uint32_t BestDistance = MaxDistance + 1;

        for (const FSpecifierInfo& Candidate : GetSpecifiers(Target))
        {
            const uint32_t Distance = EditDistance(Key, Candidate.Name);
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                Best = &Candidate;
            }
        }

        return BestDistance <= MaxDistance ? Best : nullptr;
    }

    void ValidateSpecifiers(const CXCursor& Cursor, ESpecifierTarget Target, const std::vector<FMetadataPair>& Metadata)
    {
        const char* MacroName = SpecifierTargetToString(Target);

        for (const FMetadataPair& Pair : Metadata)
        {
            if (Pair.Key.empty() || FindSpecifier(Target, Pair.Key) != nullptr)
            {
                continue;
            }

            if (const FSpecifierInfo* Suggestion = SuggestSpecifier(Target, Pair.Key))
            {
                LRT_WARNING(Cursor, EDiagId::UnknownSpecifier,
                    "%s specifier '%s' is not recognized and will be ignored. Did you mean '%s'?",
                    MacroName, Pair.Key.c_str(), std::string(Suggestion->Name).c_str());
            }
            else
            {
                LRT_WARNING(Cursor, EDiagId::UnknownSpecifier,
                    "%s specifier '%s' is not recognized and will be ignored. Add it to ReflectionSpecifiers.h if it is intentional.",
                    MacroName, Pair.Key.c_str());
            }
        }
    }
}
