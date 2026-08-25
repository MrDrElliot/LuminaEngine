#pragma once
#include "ClangParserContext.h"

namespace Lumina::Reflection
{
    class FClangParser
    {
    public:

        FClangParser() = default;

        bool Parse(FReflectedWorkspace* Workspace);

        // Promotes clang errors in reflected headers from a warning to a build failure.
        bool bStrictParse = false;
        // Reports severe diagnostics from headers we do not reflect, which are otherwise dropped.
        bool bVerboseDiagnostics = false;

        FClangParserContext ParsingContext;
        
    };
}
