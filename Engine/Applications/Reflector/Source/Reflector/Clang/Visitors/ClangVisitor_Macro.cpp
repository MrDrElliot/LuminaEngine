#include <array>
#include <string_view>
#include <utility>
#include <clang-c/CXSourceLocation.h>
#include <clang-c/Index.h>
#include <Reflector/ReflectionConfig.h>
#include <Reflector/ReflectionCore/ReflectionMacro.h>
#include "Reflector/Clang/ClangParserContext.h"
#include "Reflector/Clang/Utils.h"

namespace Lumina::Reflection::Visitor
{
    namespace
    {
        struct FMacroName
        {
            std::string_view Name;
            EReflectionMacro Macro;
        };

        constexpr std::array kReflectionMacroNames =
        {
            FMacroName{ ReflectionEnumToString(EReflectionMacro::Property),      EReflectionMacro::Property      },
            FMacroName{ ReflectionEnumToString(EReflectionMacro::Function),      EReflectionMacro::Function      },
            FMacroName{ ReflectionEnumToString(EReflectionMacro::Reflect),       EReflectionMacro::Reflect       },
            FMacroName{ ReflectionEnumToString(EReflectionMacro::GeneratedBody), EReflectionMacro::GeneratedBody },
            FMacroName{ ReflectionEnumToString(EReflectionMacro::ScriptExport),  EReflectionMacro::ScriptExport  },
        };

        // Every macro expansion in a reflected header reaches this, so it stays allocation-free.
        constexpr bool TryClassifyMacro(std::string_view Spelling, EReflectionMacro& OutMacro)
        {
            for (const FMacroName& Known : kReflectionMacroNames)
            {
                if (Known.Name == Spelling)
                {
                    OutMacro = Known.Macro;
                    return true;
                }
            }
            return false;
        }
    }

    CXChildVisitResult VisitMacro(const CXCursor& Cursor, CXCursor, FClangParserContext* Context)
    {
        EReflectionMacro MacroType = EReflectionMacro::Size;
        if (!TryClassifyMacro(ClangUtils::CursorDisplayName(Cursor).View(), MacroType))
        {
            return CXChildVisit_Continue;
        }

        // SCRIPT_EXPORT is exempt, since a free function needs nothing the header itself must include.
        if (MacroType != EReflectionMacro::ScriptExport)
        {
            Context->ReflectedHeader->bHasReflectionMacros = true;
        }

        FReflectionMacro Macro(Context->ReflectedHeader, Cursor, clang_getCursorExtent(Cursor), MacroType);
        if (MacroType == EReflectionMacro::GeneratedBody)
        {
            Context->AddGeneratedBodyMacro(std::move(Macro));
        }
        else
        {
            Context->AddReflectedMacro(std::move(Macro));
        }

        return CXChildVisit_Continue;
    }
}
