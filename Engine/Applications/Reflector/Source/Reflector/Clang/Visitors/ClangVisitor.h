#pragma once
#include <clang-c/Index.h>

namespace Lumina::Reflection
{
    class FClangParserContext;
}

namespace Lumina::Reflection::Visitor
{
    CXChildVisitResult VisitMacro(const CXCursor& Cursor, CXCursor Parent, FClangParserContext* Context);
    CXChildVisitResult VisitEnum(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context);
    CXChildVisitResult VisitStructure(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context);
    // REFLECT'd type alias: reflects the record it names, under the alias's own name.
    CXChildVisitResult VisitTypeAlias(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context);
    // REFLECT'd class template: recorded so its instantiations reflect wherever a property names one.
    CXChildVisitResult VisitClassTemplate(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context);
    CXChildVisitResult VisitClass(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context);
    // Namespace-scope free function carrying a SCRIPT_EXPORT macro (skipped otherwise).
    CXChildVisitResult VisitFunction(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context);
}
