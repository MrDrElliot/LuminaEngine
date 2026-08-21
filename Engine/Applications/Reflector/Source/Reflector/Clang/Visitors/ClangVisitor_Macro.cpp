#include <utility>
#include <clang-c/CXSourceLocation.h>
#include <clang-c/Index.h>
#include <string>
#include <Reflector/ReflectionConfig.h>
#include <Reflector/ReflectionCore/ReflectionMacro.h>
#include <unordered_map>
#include "Reflector/Clang/ClangParserContext.h"
#include "Reflector/Clang/Utils.h"

namespace Lumina::Reflection::Visitor
{

    static const std::unordered_map<std::string, EReflectionMacro> MacroMap =
    {
        { ReflectionEnumToString(EReflectionMacro::Property),      EReflectionMacro::Property },
        { ReflectionEnumToString(EReflectionMacro::Function),      EReflectionMacro::Function },
        { ReflectionEnumToString(EReflectionMacro::Reflect),       EReflectionMacro::Reflect  },
        { ReflectionEnumToString(EReflectionMacro::GeneratedBody), EReflectionMacro::GeneratedBody },
        { ReflectionEnumToString(EReflectionMacro::ScriptExport),  EReflectionMacro::ScriptExport }
    };
    
    CXChildVisitResult VisitMacro(const CXCursor& Cursor, CXCursor, FClangParserContext* Context)
    {
        std::string CursorName = ClangUtils::GetCursorDisplayName(Cursor);
        CXSourceRange Range = clang_getCursorExtent(Cursor);

        auto It = MacroMap.find(CursorName);
        if (It != MacroMap.end())
        {
            // SCRIPT_EXPORT is exempt, since a free function needs nothing the header itself must include.
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
