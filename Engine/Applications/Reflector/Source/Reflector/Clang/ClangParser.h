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

        FClangParserContext ParsingContext;
        
    };
}
