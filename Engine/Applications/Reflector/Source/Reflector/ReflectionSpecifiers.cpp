#include "ReflectionSpecifiers.h"

#include "EASTL/algorithm.h"
#include "EASTL/vector.h"
#include "Reflector/Diagnostics/LRTDiagnostics.h"

namespace Lumina::Reflection
{
    namespace
    {
#define LUMINA_SPECIFIER_ROW(Name, Form, Consumer, Doc) \
    { #Name, ESpecifierForm::Form, ESpecifierConsumer::Consumer, Doc },

        constexpr FSpecifierInfo GReflectSpecifiers[] = { LUMINA_REFLECT_SPECIFIERS(LUMINA_SPECIFIER_ROW) };
        constexpr FSpecifierInfo GPropertySpecifiers[] = { LUMINA_PROPERTY_SPECIFIERS(LUMINA_SPECIFIER_ROW) };
        constexpr FSpecifierInfo GFunctionSpecifiers[] = { LUMINA_FUNCTION_SPECIFIERS(LUMINA_SPECIFIER_ROW) };
        constexpr FSpecifierInfo GScriptExportSpecifiers[] = { LUMINA_SCRIPT_EXPORT_SPECIFIERS(LUMINA_SPECIFIER_ROW) };

#undef LUMINA_SPECIFIER_ROW

        uint32_t EditDistance(const eastl::string& A, const eastl::string& B)
        {
            eastl::vector<uint32_t> Row(B.length() + 1);
            for (uint32_t J = 0; J <= B.length(); ++J)
            {
                Row[J] = J;
            }

            for (uint32_t I = 1; I <= A.length(); ++I)
            {
                uint32_t Diagonal = Row[0];
                Row[0] = I;

                for (uint32_t J = 1; J <= B.length(); ++J)
                {
                    const uint32_t Previous = Row[J];
                    const uint32_t Cost = (A[I - 1] == B[J - 1]) ? 0u : 1u;
                    Row[J] = eastl::min(eastl::min(Row[J] + 1, Row[J - 1] + 1), Diagonal + Cost);
                    Diagonal = Previous;
                }
            }

            return Row[B.length()];
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

    const FSpecifierInfo* GetSpecifiers(ESpecifierTarget Target, uint32_t& OutCount)
    {
        switch (Target)
        {
            case ESpecifierTarget::Reflect:
                OutCount = (uint32_t)eastl::size(GReflectSpecifiers);
                return GReflectSpecifiers;

            case ESpecifierTarget::Property:
                OutCount = (uint32_t)eastl::size(GPropertySpecifiers);
                return GPropertySpecifiers;

            case ESpecifierTarget::Function:
                OutCount = (uint32_t)eastl::size(GFunctionSpecifiers);
                return GFunctionSpecifiers;

            case ESpecifierTarget::ScriptExport:
                OutCount = (uint32_t)eastl::size(GScriptExportSpecifiers);
                return GScriptExportSpecifiers;

            default:
                OutCount = 0;
                return nullptr;
        }
    }

    const FSpecifierInfo* FindSpecifier(ESpecifierTarget Target, const eastl::string& Key)
    {
        uint32_t Count = 0;
        const FSpecifierInfo* Table = GetSpecifiers(Target, Count);

        for (uint32_t Index = 0; Index < Count; ++Index)
        {
            if (Key == Table[Index].Name)
            {
                return &Table[Index];
            }
        }

        return nullptr;
    }

    const FSpecifierInfo* SuggestSpecifier(ESpecifierTarget Target, const eastl::string& Key)
    {
        uint32_t Count = 0;
        const FSpecifierInfo* Table = GetSpecifiers(Target, Count);

        // Loose enough to catch a plausible misspelling, tight enough that unrelated names never match.
        const uint32_t MaxDistance = Key.length() <= 4 ? 1u : (Key.length() <= 8 ? 2u : 3u);

        const FSpecifierInfo* Best = nullptr;
        uint32_t BestDistance = MaxDistance + 1;

        for (uint32_t Index = 0; Index < Count; ++Index)
        {
            const uint32_t Distance = EditDistance(Key, eastl::string(Table[Index].Name));
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                Best = &Table[Index];
            }
        }

        return BestDistance <= MaxDistance ? Best : nullptr;
    }

    void ValidateSpecifiers(const CXCursor& Cursor, ESpecifierTarget Target, const eastl::vector<FMetadataPair>& Metadata)
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
                    MacroName, Pair.Key.c_str(), Suggestion->Name);
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
