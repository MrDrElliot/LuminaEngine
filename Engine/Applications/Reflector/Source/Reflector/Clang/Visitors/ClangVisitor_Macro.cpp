#include <utility>
#include <clang-c/CXSourceLocation.h>
#include <clang-c/Index.h>
#include <EASTL/string.h>
#include <Reflector/ReflectionConfig.h>
#include <Reflector/ReflectionCore/ReflectionMacro.h>
#include "EASTL/fixed_hash_map.h"
#include "Reflector/Clang/ClangParserContext.h"
#include "Reflector/Clang/Utils.h"

namespace Lumina::Reflection::Visitor
{

    static const eastl::fixed_hash_map<eastl::string, EReflectionMacro, 8> MacroMap =
    {
        { ReflectionEnumToString(EReflectionMacro::Property),      EReflectionMacro::Property },
        { ReflectionEnumToString(EReflectionMacro::Function),      EReflectionMacro::Function },
        { ReflectionEnumToString(EReflectionMacro::Reflect),       EReflectionMacro::Reflect  },
        { ReflectionEnumToString(EReflectionMacro::GeneratedBody), EReflectionMacro::GeneratedBody },
        { ReflectionEnumToString(EReflectionMacro::ScriptExport),  EReflectionMacro::ScriptExport }
    };
    
    CXChildVisitResult VisitMacro(const CXCursor& Cursor, CXCursor, FClangParserContext* Context)
    {
        eastl::string CursorName = ClangUtils::GetCursorDisplayName(Cursor);
        CXSourceRange Range = clang_getCursorExtent(Cursor);

        auto It = MacroMap.find(CursorName);
        if (It != MacroMap.end())
        {
            // Record the header so the post-parse pass knows to validate its include block (must contain
            // <stem>.generated.h, must be last). SCRIPT_EXPORT is exempt: a free function emits a thunk into
            // the .generated.cpp but nothing the header itself must include, so a SCRIPT_EXPORT-only header
            // needs no .generated.h.
            if (It->second != EReflectionMacro::ScriptExport)
            {
                Context->ReflectedHeader->bHasReflectionMacros = true;
            }

            FReflectionMacro Macro(Context->ReflectedHeader->HeaderPath, Cursor, Range, It->second);
            if (It->second == EReflectionMacro::GeneratedBody)
            {
                Context->AddGeneratedBodyMacro(std::move(Macro));
            }
            else
            {
                Context->AddReflectedMacro(std::move(Macro));
            }
        }
        
        return CXChildVisit_Continue;

    }
}
